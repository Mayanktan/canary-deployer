#include "deployer.h"
#include "events.h"
#include "instances.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define READY_TIMEOUT 15   // seconds a new instance gets to pass its health checks
#define MAX_HISTORY   100  // deployments kept in memory (the file keeps all of them)

typedef struct {
    int    id;
    char   from[VERSION_MAX + 1];
    char   to[VERSION_MAX + 1];
    char   phase[16];  // canary, rolling out, succeeded, rolled back
    char   reason[200];
    time_t started_at;
    time_t finished_at;
} Deployment;

// Everything below is protected by deploy_lock.
static pthread_mutex_t deploy_lock = PTHREAD_MUTEX_INITIALIZER;
static char       stable[VERSION_MAX + 1];    // the version users should be getting
static char       previous[VERSION_MAX + 1];  // where `rollback` goes back to
static Deployment current;                    // the running (or most recent) deployment
static Deployment history[MAX_HISTORY];
static int        history_count;
static int        next_id = 1;
static int        in_progress;
static pthread_t  deploy_thread_id;
static int        has_deploy_thread;

static atomic_int abort_requested;  // read inside loops that hold instances_lock, so it is atomic instead
static char       state_path[512], history_path[512];

// ---------------------------------------------------------------------------
// Saving to disk
// ---------------------------------------------------------------------------

// Saves the state file safely: write a temporary file, flush it to the disk
// with fsync(), then rename() it over the old one. rename() is atomic, so a
// crash leaves either the old file or the new one, never half of each.
static void save_state(void) {
    char tmp_path[600];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", state_path);
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        log_event("could not save state: %s", strerror(errno));
        return;
    }
    fprintf(f, "stable %s\nnext_id %d\n", stable, next_id);
    if (previous[0]) fprintf(f, "previous %s\n", previous);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) log_event("could not save state: %s", strerror(errno));
    fclose(f);
    rename(tmp_path, state_path);
}

static void load_state(const char *initial_version) {
    snprintf(stable, sizeof stable, "%s", initial_version);
    FILE *f = fopen(state_path, "r");
    if (!f) return;  // first run
    char key[16], value[VERSION_MAX + 1];
    while (fscanf(f, "%15s %32s", key, value) == 2) {
        if (strcmp(key, "stable") == 0)   snprintf(stable, sizeof stable, "%s", value);
        if (strcmp(key, "previous") == 0) snprintf(previous, sizeof previous, "%s", value);
        if (strcmp(key, "next_id") == 0)  next_id = atoi(value);
    }
    fclose(f);
}

static void remember(const Deployment *d) {
    if (history_count == MAX_HISTORY) {  // drop the oldest
        memmove(history, history + 1, (MAX_HISTORY - 1) * sizeof *history);
        history_count--;
    }
    history[history_count++] = *d;
}

// The history file is append-only: one line per finished deployment.
static void append_history(const Deployment *d) {
    remember(d);
    FILE *f = fopen(history_path, "a");
    if (!f) return;
    fprintf(f, "%d|%s|%s|%s|%ld|%ld|%s\n", d->id, d->from, d->to, d->phase,
            (long)d->started_at, (long)d->finished_at, d->reason);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void load_history(void) {
    FILE *f = fopen(history_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        Deployment d;
        memset(&d, 0, sizeof d);
        long started, finished;
        if (sscanf(line, "%d|%32[^|]|%32[^|]|%15[^|]|%ld|%ld|%199[^\n]", &d.id, d.from, d.to, d.phase,
                   &started, &finished, d.reason) >= 6) {
            d.started_at = started;
            d.finished_at = finished;
            remember(&d);
        }
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// The deployment steps. They run on the deployment's own thread.
// ---------------------------------------------------------------------------

static void set_phase(const char *phase) {
    pthread_mutex_lock(&deploy_lock);
    snprintf(current.phase, sizeof current.phase, "%s", phase);
    pthread_mutex_unlock(&deploy_lock);
}

// Waits until the instance in `slot` passes its health checks.
// Caller holds instances_lock. Returns 1 if ready, or 0 and fills in `reason`.
static int wait_until_ready(int slot, char *reason, size_t size) {
    Instance *in = &instances[slot];
    time_t deadline = time(NULL) + READY_TIMEOUT;
    while (in->state == STARTING && !abort_requested && instances_wait(deadline)) {}

    if (in->state == READY) return 1;
    if (abort_requested)          snprintf(reason, size, "aborted by user");
    else if (in->state == CRASHED) snprintf(reason, size, "%s crashed while starting", in->version);
    else snprintf(reason, size, "%s did not pass its health checks within %d s", in->version, READY_TIMEOUT);
    return 0;
}

// Starts instances of `version` until `count` are running, then waits for them to be ready.
static void ensure_running(const char *version, int count) {
    pthread_mutex_lock(&instances_lock);
    while (instances_count(version) < count && instance_start(version, 0) >= 0) {}
    time_t deadline = time(NULL) + READY_TIMEOUT;
    while (instances_ready(version) < count && instances_wait(deadline)) {}
    pthread_mutex_unlock(&instances_lock);
}

// Step 1, the canary: start ONE instance of the new version next to the old
// ones. The load balancer gives it its share of real traffic (1 of 4
// instances = about 25%), and we watch its error rate before trusting it.
//
// Health checks alone are not enough: a buggy release can answer /health
// fine and still fail real requests. Watching real traffic catches that.
static int run_canary(const char *version, char *reason, size_t size) {
    pthread_mutex_lock(&instances_lock);
    int slot = instance_start(version, 1);
    int ok = slot >= 0 && wait_until_ready(slot, reason, size);
    if (slot < 0) snprintf(reason, size, "could not start the canary");

    if (ok) {
        Instance *canary = &instances[slot];
        log_event("canary %s is live on port %d, watching its traffic for %d s",
                  version, canary->port, config.canary_seconds);
        time_t end = time(NULL) + config.canary_seconds;
        while (ok && time(NULL) < end) {
            long error_pct = canary->requests ? canary->errors * 100 / canary->requests : 0;
            if (abort_requested) {
                ok = 0;
                snprintf(reason, size, "aborted by user");
            } else if (canary->state == CRASHED) {
                ok = 0;
                snprintf(reason, size, "canary crashed");
            } else if (canary->state != READY) {
                ok = 0;
                snprintf(reason, size, "canary started failing health checks");
            } else if (canary->requests >= config.min_requests && error_pct > config.max_error_pct) {
                ok = 0;
                snprintf(reason, size, "canary error rate %ld%% is above the %d%% limit (%ld of %ld requests failed)",
                         error_pct, config.max_error_pct, canary->errors, canary->requests);
            } else {
                instances_wait(end);  // wakes up on every finished request or state change
            }
        }
        if (ok) {
            if (canary->requests < config.min_requests) {
                log_event("canary passed (only %ld requests, so judged on health checks alone)", canary->requests);
            } else {
                log_event("canary passed: %ld requests, %ld%% errors", canary->requests,
                          canary->errors * 100 / canary->requests);
            }
            canary->is_canary = 0;  // it is now a normal instance of the new version
            canary->restart_on_crash = 1;
        }
    }
    pthread_mutex_unlock(&instances_lock);

    if (!ok && slot >= 0) instance_stop(slot);  // the old version never stopped serving
    return ok;
}

// A slot running something other than `version`, or -1. Caller holds instances_lock.
static int find_old_instance(const char *version) {
    for (int i = 0; i < MAX_INSTANCES; i++) {
        InstanceState s = instances[i].state;
        if (s != EMPTY && s != STOPPING && strcmp(instances[i].version, version) != 0) return i;
    }
    return -1;
}

// Step 2, the rolling update: replace the old instances one at a time.
// Each new instance must be healthy before an old one is stopped, so the
// number of instances serving traffic never drops.
static int roll_out(const char *version, char *reason, size_t size) {
    while (!abort_requested) {
        pthread_mutex_lock(&instances_lock);
        int old = find_old_instance(version);
        int ok = 1;
        if (old >= 0 && instances_count(version) < config.instance_count) {
            int slot = instance_start(version, 0);
            ok = slot >= 0 && wait_until_ready(slot, reason, size);
            if (slot < 0) snprintf(reason, size, "could not start a new instance");
        }
        pthread_mutex_unlock(&instances_lock);

        if (old < 0) return 1;  // nothing old left: done
        if (!ok) return 0;
        instance_stop(old);
    }
    snprintf(reason, size, "aborted by user");
    return 0;
}

// Puts the old version back: start old instances until there are enough
// again, then remove every instance of the failed version.
static void roll_back(const char *bad_version, const char *good_version) {
    if (running) ensure_running(good_version, config.instance_count);
    for (int slot = 0; slot < MAX_INSTANCES; slot++) {
        pthread_mutex_lock(&instances_lock);
        int bad = instances[slot].state != EMPTY && strcmp(instances[slot].version, bad_version) == 0;
        pthread_mutex_unlock(&instances_lock);
        if (bad) instance_stop(slot);
    }
}

static void *deploy_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&deploy_lock);
    int  id = current.id;
    char from[VERSION_MAX + 1], to[VERSION_MAX + 1];
    snprintf(from, sizeof from, "%s", current.from);
    snprintf(to, sizeof to, "%s", current.to);
    pthread_mutex_unlock(&deploy_lock);

    char reason[200] = "";
    log_event("deployment #%d started: %s -> %s", id, from, to);
    set_phase("canary");
    int ok = run_canary(to, reason, sizeof reason);
    if (ok) {
        set_phase("rolling out");
        ok = roll_out(to, reason, sizeof reason);
    }
    if (!ok) {
        log_event("deployment #%d failed: %s. Rolling back to %s", id, reason, from);
        roll_back(to, from);
    }

    pthread_mutex_lock(&deploy_lock);
    current.finished_at = time(NULL);
    snprintf(current.phase, sizeof current.phase, "%s", ok ? "succeeded" : "rolled back");
    snprintf(current.reason, sizeof current.reason, "%s", ok ? "" : reason);
    if (ok) {
        snprintf(previous, sizeof previous, "%s", from);
        snprintf(stable, sizeof stable, "%s", to);
    }
    save_state();
    append_history(&current);
    in_progress = 0;
    pthread_mutex_unlock(&deploy_lock);

    if (ok) log_event("deployment #%d succeeded: all %d instances now run %s", id, config.instance_count, to);
    else    log_event("deployment #%d rolled back: users are on %s", id, from);
    return NULL;
}

// ---------------------------------------------------------------------------
// Called by main.c and the API
// ---------------------------------------------------------------------------

int deployer_init(const char *initial_version) {
    snprintf(state_path, sizeof state_path, "%s/state.txt", config.data_dir);
    snprintf(history_path, sizeof history_path, "%s/history.txt", config.data_dir);
    load_state(initial_version);
    load_history();
    if (history_count) current = history[history_count - 1];  // so `status` can show the last deployment

    if (!release_exists(stable)) {
        fprintf(stderr, "release '%s' not found: expected an executable %s/%s.sh\n",
                stable, config.releases_dir, stable);
        return -1;
    }
    log_event("starting %d instances of %s", config.instance_count, stable);
    ensure_running(stable, config.instance_count);
    return 0;
}

void deployer_shutdown(void) {
    pthread_mutex_lock(&deploy_lock);
    if (in_progress) abort_requested = 1;
    int join = has_deploy_thread;
    has_deploy_thread = 0;
    pthread_mutex_unlock(&deploy_lock);

    pthread_mutex_lock(&instances_lock);
    pthread_cond_broadcast(&instances_changed);  // wake the deployment so it sees the abort
    pthread_mutex_unlock(&instances_lock);
    if (join) pthread_join(deploy_thread_id, NULL);
}

int deployer_deploy(const char *version, char *message, size_t size) {
    if (!release_exists(version)) {
        snprintf(message, size, "no release named '%s' (see: deployctl releases)", version);
        return 404;
    }

    int status;
    pthread_mutex_lock(&deploy_lock);
    if (in_progress) {
        snprintf(message, size, "deployment #%d (%s -> %s) is still running", current.id, current.from, current.to);
        status = 409;
    } else if (strcmp(version, stable) == 0) {
        snprintf(message, size, "%s is already deployed", version);
        status = 409;
    } else {
        // The previous deployment's thread has finished; collect it before starting a new one.
        if (has_deploy_thread) pthread_join(deploy_thread_id, NULL);

        memset(&current, 0, sizeof current);
        current.id = next_id++;
        snprintf(current.from, sizeof current.from, "%s", stable);
        snprintf(current.to, sizeof current.to, "%s", version);
        snprintf(current.phase, sizeof current.phase, "starting");
        current.started_at = time(NULL);
        in_progress = 1;
        abort_requested = 0;
        pthread_create(&deploy_thread_id, NULL, deploy_thread, NULL);
        has_deploy_thread = 1;

        snprintf(message, size, "deployment #%d started: %s -> %s (canary first)", current.id, current.from, version);
        status = 202;  // "Accepted": it runs in the background; follow it with GET /status
    }
    pthread_mutex_unlock(&deploy_lock);
    return status;
}

int deployer_rollback(char *message, size_t size) {
    char target[VERSION_MAX + 1];
    pthread_mutex_lock(&deploy_lock);
    snprintf(target, sizeof target, "%s", previous);
    pthread_mutex_unlock(&deploy_lock);

    if (!target[0]) {
        snprintf(message, size, "there is no previous version to roll back to");
        return 409;
    }
    return deployer_deploy(target, message, size);
}

int deployer_abort(char *message, size_t size) {
    int status = 202;
    pthread_mutex_lock(&deploy_lock);
    if (!in_progress) {
        snprintf(message, size, "no deployment is running");
        status = 409;
    } else {
        abort_requested = 1;
        snprintf(message, size, "aborting deployment #%d, rolling back to %s", current.id, current.from);
    }
    pthread_mutex_unlock(&deploy_lock);

    pthread_mutex_lock(&instances_lock);
    pthread_cond_broadcast(&instances_changed);  // wake the deployment thread so it notices
    pthread_mutex_unlock(&instances_lock);
    return status;
}

static void deployment_json(StrBuf *out, const Deployment *d) {
    time_t end = d->finished_at ? d->finished_at : time(NULL);
    sb_printf(out, "{\"id\":%d,\"from\":\"%s\",\"to\":\"%s\",\"phase\":\"%s\",\"started\":%ld,\"duration\":%ld,\"reason\":",
              d->id, d->from, d->to, d->phase, (long)d->started_at, (long)(end - d->started_at));
    sb_json_string(out, d->reason);
    sb_printf(out, "}");
}

void deployer_status_json(StrBuf *out) {
    pthread_mutex_lock(&deploy_lock);
    sb_printf(out, "{\"stable\":\"%s\",\"previous\":\"%s\",\"in_progress\":%s,\"current\":",
              stable, previous, in_progress ? "true" : "false");
    if (current.id) deployment_json(out, &current);
    else            sb_printf(out, "null");
    sb_printf(out, "}");
    pthread_mutex_unlock(&deploy_lock);
}

void deployer_history_json(StrBuf *out) {
    pthread_mutex_lock(&deploy_lock);
    sb_printf(out, "{\"deployments\":[");
    for (int i = history_count - 1; i >= 0; i--) {
        if (i != history_count - 1) sb_printf(out, ",");
        deployment_json(out, &history[i]);
    }
    sb_printf(out, "]}");
    pthread_mutex_unlock(&deploy_lock);
}
