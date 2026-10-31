// controller: runs several copies (instances) of a service, load-balances
// traffic across them, keeps them healthy, and deploys new versions safely:
// canary first, then a rolling update, with automatic rollback on failure.
//
// Threads:
//   main        startup, then waits for Ctrl+C / SIGTERM and shuts down
//   supervisor  waitpid(): notices when an instance process exits or crashes
//   monitor     health checks every second, restarts crashed instances
//   proxy       accepts user connections; a pool of workers forwards them
//   api         serves deployctl
//   deployment  one per deployment, runs canary -> rollout -> (rollback)

#include "api.h"
#include "controller.h"
#include "deployer.h"
#include "events.h"
#include "instances.h"
#include "proxy.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROXY_WORKERS 16

Config config = {
    .instance_count = 3,
    .proxy_port = 8080,
    .api_port = 8081,
    .base_port = 9001,
    .canary_seconds = 20,
    .max_error_pct = 5,
    .min_requests = 20,
    .releases_dir = "releases",
    .data_dir = "data",
};
atomic_int running = 1;

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -v VERSION  version to run on first start (default v1)\n"
            "  -n COUNT    number of instances (default 3)\n"
            "  -p PORT     load balancer port, where users send traffic (default 8080)\n"
            "  -a PORT     deploy API port, used by deployctl (default 8081)\n"
            "  -b PORT     port of the first instance (default 9001)\n"
            "  -w SECONDS  how long to watch a canary (default 20)\n"
            "  -e PERCENT  canary error rate that triggers a rollback (default 5)\n"
            "  -r DIR      releases folder (default releases)\n"
            "  -d DIR      folder for state, history and logs (default data)\n",
            program);
    exit(2);
}

int main(int argc, char *argv[]) {
    const char *initial_version = "v1";
    int opt;
    while ((opt = getopt(argc, argv, "v:n:p:a:b:w:e:r:d:h")) != -1) {
        switch (opt) {
            case 'v': initial_version = optarg; break;
            case 'n': config.instance_count = atoi(optarg); break;
            case 'p': config.proxy_port = atoi(optarg); break;
            case 'a': config.api_port = atoi(optarg); break;
            case 'b': config.base_port = atoi(optarg); break;
            case 'w': config.canary_seconds = atoi(optarg); break;
            case 'e': config.max_error_pct = atoi(optarg); break;
            case 'r': snprintf(config.releases_dir, sizeof config.releases_dir, "%s", optarg); break;
            case 'd': snprintf(config.data_dir, sizeof config.data_dir, "%s", optarg); break;
            default: usage(argv[0]);
        }
    }
    // A rolling update briefly runs old and new instances side by side.
    if (config.instance_count < 1 || config.instance_count > MAX_INSTANCES / 2) {
        fprintf(stderr, "number of instances must be between 1 and %d\n", MAX_INSTANCES / 2);
        return 2;
    }

    char path[600];
    mkdir(config.data_dir, 0755);
    snprintf(path, sizeof path, "%s/logs", config.data_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/events.log", config.data_dir);
    events_open(path);

    // Block SIGINT and SIGTERM in this thread before creating any other, so
    // every thread inherits the block. main() then waits for them with
    // sigwait(): no signal handler, and no thread is interrupted at a bad moment.
    // (A program started in the background by a script inherits SIGINT as
    // "ignored", and ignored signals are thrown away, so reset them first.)
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);
    signal(SIGPIPE, SIG_IGN);  // writing to a closed socket returns an error instead of killing us

    instances_init();
    pthread_t supervisor, monitor;
    pthread_create(&supervisor, NULL, supervisor_thread, NULL);
    pthread_create(&monitor, NULL, monitor_thread, NULL);

    int started = deployer_init(initial_version) == 0 && proxy_start(PROXY_WORKERS) == 0 && api_start() == 0;
    if (started) {
        printf("\nController is up.\n"
               "  Users:        http://localhost:%d    (the load balancer)\n"
               "  deployctl:    http://localhost:%d    (deploy API)\n"
               "  Instances:    ports %d and up, logs in %s/logs/\n"
               "Press Ctrl+C to stop.\n\n",
               config.proxy_port, config.api_port, config.base_port, config.data_dir);
        int sig;
        sigwait(&stop_signals, &sig);
        log_event("received %s, shutting down", sig == SIGINT ? "SIGINT" : "SIGTERM");
    }

    // Shut down in order: stop taking new work, finish what's running,
    // then stop the instances while the supervisor is still collecting them.
    running = 0;
    deployer_shutdown();
    proxy_stop();
    api_stop();
    pthread_join(monitor, NULL);
    instances_stop_all();
    pthread_join(supervisor, NULL);
    log_event("controller stopped");
    events_close();
    return started ? 0 : 1;
}
