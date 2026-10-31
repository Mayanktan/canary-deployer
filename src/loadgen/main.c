// loadgen: plays the part of real users. It sends a steady stream of requests
// to the load balancer and prints, every second, which versions answered and
// how many requests failed.
//
//   ./bin/loadgen                 about 50 requests/s until Ctrl+C
//   ./bin/loadgen -r 200 -d 30    about 200 requests/s for 30 seconds

#include "../common/http.h"
#include "../common/json.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_VERSIONS 16
#define WORKERS      8

typedef struct {
    char name[40];
    long ok;      // answered with 2xx
    long errors;  // answered with 5xx by the service itself (a bug in that version)
} VersionCount;

static pthread_mutex_t counts_lock = PTHREAD_MUTEX_INITIALIZER;
static VersionCount    versions[MAX_VERSIONS];
static int             version_count;
static long            unavailable;  // nobody answered: what users would call "the site is down"
static long            total;

static volatile sig_atomic_t stop = 0;
static const char *host = "127.0.0.1";
static int         port = 8080;
static int         pause_us;  // each worker's pause between requests

static void on_signal(int sig) {
    (void)sig;
    stop = 1;
}

static void record(const char *version, int status) {
    pthread_mutex_lock(&counts_lock);
    total++;
    if (!version[0]) {
        unavailable++;  // no answer, or the load balancer itself answered (502/503/504)
    } else {
        int i = 0;
        while (i < version_count && strcmp(versions[i].name, version) != 0) i++;
        if (i == version_count && version_count < MAX_VERSIONS) {
            snprintf(versions[version_count++].name, sizeof versions[0].name, "%s", version);
        }
        if (i < version_count) {
            if (status < 500) versions[i].ok++;
            else              versions[i].errors++;
        }
    }
    pthread_mutex_unlock(&counts_lock);
}

static void *worker(void *arg) {
    (void)arg;
    while (!stop) {
        HttpResponse res = http_call(host, port, "GET", "/", NULL, 5000);
        char version[40] = "";
        json_get_string(res.body, "version", version, sizeof version);  // the service says who answered
        record(version, res.status);
        http_free(&res);
        usleep((useconds_t)pause_us);
    }
    return NULL;
}

static void print_counts(void) {
    for (int i = 0; i < version_count; i++) {
        printf("  %s: %ld ok", versions[i].name, versions[i].ok);
        if (versions[i].errors) printf(", %ld failed", versions[i].errors);
    }
    printf("  | unavailable: %ld\n", unavailable);
}

int main(int argc, char *argv[]) {
    int rate = 50, duration = 0, opt;
    while ((opt = getopt(argc, argv, "H:p:r:d:")) != -1) {
        switch (opt) {
            case 'H': host = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'r': rate = atoi(optarg); break;
            case 'd': duration = atoi(optarg); break;
            default:
                fprintf(stderr, "usage: loadgen [-H host] [-p port] [-r requests_per_second] [-d seconds]\n");
                return 2;
        }
    }
    if (rate < 1) rate = 1;
    pause_us = WORKERS * 1000000 / rate;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    printf("Sending about %d requests/s to %s:%d. Ctrl+C to stop.\n", rate, host, port);
    pthread_t threads[WORKERS];
    for (int i = 0; i < WORKERS; i++) pthread_create(&threads[i], NULL, worker, NULL);

    long last_total = 0;
    for (int second = 1; !stop && (duration == 0 || second <= duration); second++) {
        sleep(1);
        char now[16];
        time_t t = time(NULL);
        strftime(now, sizeof now, "%H:%M:%S", localtime(&t));

        pthread_mutex_lock(&counts_lock);
        printf("%s %4ld req/s", now, total - last_total);
        print_counts();
        last_total = total;
        pthread_mutex_unlock(&counts_lock);
        fflush(stdout);
    }

    stop = 1;
    for (int i = 0; i < WORKERS; i++) pthread_join(threads[i], NULL);
    printf("\nTotal: %ld requests\n", total);
    print_counts();
    return 0;
}
