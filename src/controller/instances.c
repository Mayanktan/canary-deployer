#include "instances.h"
#include "events.h"
#include "../common/http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define HEALTH_TIMEOUT_MS 1000
#define START_TIMEOUT     15  // seconds a new instance gets to pass its first health check
#define UNHEALTHY_AFTER   3   // failed checks in a row before we stop sending it traffic
#define KILL_AFTER        10  // failed checks in a row before we kill it (it will be restarted)
#define DRAIN_SECONDS     5   // how long to wait for in-flight requests when stopping
#define GRACE_SECONDS     5   // how long to wait after SIGTERM before using SIGKILL
#define MAX_BACKOFF       30  // seconds; crash-looping instances are restarted at most this often

Instance        instances[MAX_INSTANCES];
pthread_mutex_t instances_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  instances_changed = PTHREAD_COND_INITIALIZER;

void instances_init(void) {
    for (int i = 0; i < MAX_INSTANCES; i++) instances[i].port = config.base_port + i;
}

const char *state_name(InstanceState state) {
    static const char *names[] = { "empty", "starting", "ready", "unhealthy", "stopping", "crashed" };
    return names[state];
}

int release_exists(const char *version) {
    // Only simple names, so a request can't make us run something like "../../bin/sh".
    if (!*version || strlen(version) > VERSION_MAX) return 0;
    for (const char *p = version; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_') return 0;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.sh", config.releases_dir, version);
    return access(path, X_OK) == 0;
}

// Runs the release script for this instance as a new child process.
// The caller holds instances_lock, so the supervisor can't see the child exit
// before its pid has been written into the table.
static int spawn(Instance *in) {
    char script[512], port_arg[16], log_path[600];
    snprintf(script, sizeof script, "%s/%s.sh", config.releases_dir, in->version);
    snprintf(port_arg, sizeof port_arg, "%d", in->port);
    snprintf(log_path, sizeof log_path, "%s/logs/instance-%d.log", config.data_dir, in->port);

    // Prepare everything before fork(). In a program with threads, the child
    // may only make async-signal-safe calls between fork() and exec().
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) return -1;
    sigset_t no_signals;
    sigemptyset(&no_signals);

    pid_t pid = fork();
    if (pid < 0) {
        close(log_fd);
        return -1;
    }

    if (pid == 0) {
        // ---- child process ----
        setpgid(0, 0);                // own process group: Ctrl+C in the terminal reaches only the controller
        dup2(log_fd, STDOUT_FILENO);  // the service's output goes to its log file
        dup2(log_fd, STDERR_FILENO);
        for (int fd = 3; fd < 1024; fd++) close(fd);  // don't leak the controller's sockets into the service
        signal(SIGPIPE, SIG_DFL);     // undo settings that would otherwise be inherited through exec
        sigprocmask(SIG_SETMASK, &no_signals, NULL);
        execl(script, script, port_arg, (char *)NULL);
        _exit(127);                   // only reached if exec failed
    }

    // ---- controller ----
    setpgid(pid, pid);  // also set it here (like shells do), so it's in place whichever side runs first
    close(log_fd);
    in->pid = pid;
    in->state = STARTING;
    in->failed_checks = 0;
    in->started_at = time(NULL);
    in->requests = in->errors = 0;
    pthread_cond_broadcast(&instances_changed);
    return 0;
}

int instance_start(const char *version, int is_canary) {
    for (int slot = 0; slot < MAX_INSTANCES; slot++) {
        Instance *in = &instances[slot];
        if (in->state != EMPTY) continue;

        snprintf(in->version, sizeof in->version, "%s", version);
        in->is_canary = is_canary;
        in->restart_on_crash = !is_canary;
        in->restarts = 0;
        if (spawn(in) != 0) {
            log_event("could not start %s on port %d: %s", version, in->port, strerror(errno));
            return -1;
        }
        log_event("started %s%s on port %d (pid %d)", version, is_canary ? " as canary" : "", in->port, in->pid);
        return slot;
    }
    log_event("no free slot to start %s", version);
    return -1;
}

int instances_count(const char *version) {
    int n = 0;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        InstanceState s = instances[i].state;
        if (s != EMPTY && s != STOPPING && strcmp(instances[i].version, version) == 0) n++;
    }
    return n;
}

int instances_ready(const char *version) {
    int n = 0;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (instances[i].state == READY && strcmp(instances[i].version, version) == 0) n++;
    }
    return n;
}

int instances_wait(time_t deadline) {
    struct timespec until = { .tv_sec = deadline, .tv_nsec = 0 };
    pthread_cond_timedwait(&instances_changed, &instances_lock, &until);
    return time(NULL) < deadline;
}

// Stops an instance the way Kubernetes stops a pod:
//   1. stop sending it new requests       (state = STOPPING)
//   2. let requests in progress finish    (wait for in_flight == 0)
//   3. ask the process to exit            (SIGTERM)
//   4. force it if it doesn't             (SIGKILL)
void instance_stop(int slot) {
    pthread_mutex_lock(&instances_lock);
    Instance *in = &instances[slot];
    if (in->state == EMPTY) {
        pthread_mutex_unlock(&instances_lock);
        return;
    }
    in->state = STOPPING;
    in->restart_on_crash = 0;
    pthread_cond_broadcast(&instances_changed);

    time_t deadline = time(NULL) + DRAIN_SECONDS;
    while (in->in_flight > 0 && instances_wait(deadline)) {}

    if (in->pid > 0) {
        kill(-in->pid, SIGTERM);  // a negative pid means "the whole process group"
        deadline = time(NULL) + GRACE_SECONDS;
        while (in->pid > 0 && instances_wait(deadline)) {}
    }
    if (in->pid > 0) {
        log_event("%s on port %d ignored SIGTERM, sending SIGKILL", in->version, in->port);
        kill(-in->pid, SIGKILL);
        while (in->pid > 0) instances_wait(time(NULL) + 1);  // the supervisor sets pid to 0
    }

    log_event("stopped %s on port %d", in->version, in->port);
    in->state = EMPTY;
    in->is_canary = 0;
    pthread_cond_broadcast(&instances_changed);
    pthread_mutex_unlock(&instances_lock);
}

void instances_stop_all(void) {
    for (int slot = 0; slot < MAX_INSTANCES; slot++) instance_stop(slot);
}

void instances_json(StrBuf *out) {
    pthread_mutex_lock(&instances_lock);
    time_t now = time(NULL);
    int first = 1;
    sb_printf(out, "[");
    for (int i = 0; i < MAX_INSTANCES; i++) {
        Instance *in = &instances[i];
        if (in->state == EMPTY) continue;
        sb_printf(out,
                  "%s{\"port\":%d,\"pid\":%d,\"version\":\"%s\",\"state\":\"%s\",\"canary\":%s,"
                  "\"requests\":%ld,\"errors\":%ld,\"restarts\":%d,\"uptime\":%ld}",
                  first ? "" : ",", in->port, (int)in->pid, in->version, state_name(in->state),
                  in->is_canary ? "true" : "false", in->requests, in->errors, in->restarts,
                  in->pid ? (long)(now - in->started_at) : 0L);
        first = 0;
    }
    sb_printf(out, "]");
    pthread_mutex_unlock(&instances_lock);
}

static void describe_exit(int status, char *out, size_t size) {
    if (WIFEXITED(status)) {
        snprintf(out, size, "exited with code %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(out, size, "killed by signal %d: %s", WTERMSIG(status), strsignal(WTERMSIG(status)));
    } else {
        snprintf(out, size, "stopped");
    }
}

// Waits for child processes to exit. waitpid() also "reaps" them: until the
// parent collects its exit status, a dead child stays around as a zombie.
void *supervisor_thread(void *arg) {
    (void)arg;
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == ECHILD && !running) return NULL;  // no children left and we're shutting down
            usleep(100 * 1000);                            // no children right now
            continue;
        }

        pthread_mutex_lock(&instances_lock);
        for (int i = 0; i < MAX_INSTANCES; i++) {
            Instance *in = &instances[i];
            if (in->pid != pid) continue;

            in->pid = 0;
            if (in->state != STOPPING) {  // nobody asked it to stop: it crashed
                char why[96];
                describe_exit(status, why, sizeof why);
                int backoff = in->restarts < 5 ? 1 << in->restarts : MAX_BACKOFF;  // 1, 2, 4, 8, 16, 30 s
                in->state = CRASHED;
                in->restart_at = time(NULL) + backoff;
                if (in->restart_on_crash) {
                    log_event("%s on port %d crashed (%s), restarting in %d s", in->version, in->port, why, backoff);
                } else {
                    log_event("%s on port %d crashed (%s)", in->version, in->port, why);
                }
            }
            pthread_cond_broadcast(&instances_changed);
            break;
        }
        pthread_mutex_unlock(&instances_lock);
    }
}

// Applies the result of one health check.
static void record_health(int slot, pid_t checked_pid, int healthy) {
    pthread_mutex_lock(&instances_lock);
    Instance *in = &instances[slot];
    InstanceState s = in->state;
    if (in->pid != checked_pid || (s != STARTING && s != READY && s != UNHEALTHY)) {
        pthread_mutex_unlock(&instances_lock);  // it changed while we were checking
        return;
    }

    if (healthy) {
        in->failed_checks = 0;
        if (s != READY) {
            in->state = READY;
            log_event("%s on port %d is %s", in->version, in->port, s == STARTING ? "ready" : "healthy again");
        }
    } else {
        in->failed_checks++;
        if (s == READY && in->failed_checks == UNHEALTHY_AFTER) {
            in->state = UNHEALTHY;
            log_event("%s on port %d failed %d health checks, taking it out of the load balancer",
                      in->version, in->port, UNHEALTHY_AFTER);
        }
        int stuck = (s == STARTING) ? time(NULL) - in->started_at > START_TIMEOUT
                                    : in->failed_checks >= KILL_AFTER;
        if (stuck) {
            log_event("%s on port %d is not responding, killing it", in->version, in->port);
            kill(-in->pid, SIGKILL);  // the supervisor will see it die and it gets restarted
        }
    }
    pthread_cond_broadcast(&instances_changed);
    pthread_mutex_unlock(&instances_lock);
}

// Once a second: restart crashed instances whose backoff is over, then
// health-check every running instance with GET /health.
void *monitor_thread(void *arg) {
    (void)arg;
    while (running) {
        int   slots[MAX_INSTANCES];
        pid_t pids[MAX_INSTANCES];
        int   n = 0;

        pthread_mutex_lock(&instances_lock);
        time_t now = time(NULL);
        for (int i = 0; i < MAX_INSTANCES; i++) {
            Instance *in = &instances[i];
            if (in->state == CRASHED && in->restart_on_crash && now >= in->restart_at) {
                in->restarts++;
                if (spawn(in) == 0) {
                    log_event("restarted %s on port %d (pid %d, restart #%d)", in->version, in->port, in->pid, in->restarts);
                } else {
                    in->restart_at = now + MAX_BACKOFF;
                }
            }
            if (in->state == STARTING || in->state == READY || in->state == UNHEALTHY) {
                slots[n] = i;
                pids[n] = in->pid;
                n++;
            }
        }
        pthread_mutex_unlock(&instances_lock);

        // Health checks are network calls, so make them without holding the lock.
        for (int k = 0; k < n; k++) {
            HttpResponse res = http_call("127.0.0.1", config.base_port + slots[k], "GET", "/health", NULL,
                                         HEALTH_TIMEOUT_MS);
            record_health(slots[k], pids[k], res.status == 200);
            http_free(&res);
        }
        sleep(1);
    }
    return NULL;
}
