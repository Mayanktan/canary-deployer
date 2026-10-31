// demo-service: the web service that the controller deploys.
//
// Every "release" in releases/ runs this program with different flags, so a
// release can behave badly on purpose and give the controller something to catch:
//
//   --error-rate 40     answer 40% of requests with HTTP 500 (a buggy release)
//   --crash-after 50    crash after 50 requests (an unstable release)
//   --slow-ms 300       take 300 ms longer to answer each request (a slow release)
//   --boot-seconds 3    report "not ready" for the first 3 seconds (a slow start)
//
// GET /health is the health check. Every other path is a normal request.

#include "../common/http.h"
#include "../common/net.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

static void on_sigterm(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void usage(void) {
    fprintf(stderr, "usage: demo-service --port N --version NAME [--error-rate PCT] "
                    "[--crash-after N] [--slow-ms MS] [--boot-seconds S]\n");
    exit(2);
}

int main(int argc, char *argv[]) {
    int port = 0, error_rate = 0, crash_after = 0, slow_ms = 0, boot_seconds = 0;
    const char *version = NULL;

    for (int i = 1; i + 1 < argc; i += 2) {
        if      (strcmp(argv[i], "--port") == 0)         port = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--version") == 0)      version = argv[i + 1];
        else if (strcmp(argv[i], "--error-rate") == 0)   error_rate = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--crash-after") == 0)  crash_after = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--slow-ms") == 0)      slow_ms = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--boot-seconds") == 0) boot_seconds = atoi(argv[i + 1]);
        else usage();
    }
    if (port <= 0 || !version) usage();

    // SIGTERM means "please stop": finish the current request, then exit cleanly.
    // No SA_RESTART, so a blocked call returns early and we notice the request.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = net_listen("127.0.0.1", port);
    if (listen_fd < 0) {
        perror("listen");
        return 1;
    }
    srand((unsigned)(time(NULL) ^ getpid()));
    time_t started = time(NULL);
    long served = 0;
    printf("%s listening on port %d (pid %d)\n", version, port, getpid());
    fflush(stdout);

    while (!stop_requested) {
        int client = net_accept(listen_fd, 500);
        if (client < 0) continue;
        net_set_timeout(client, 2000);

        HttpRequest req;
        if (http_read_request(client, &req) != 0) {
            close(client);
            continue;
        }

        char body[256];
        if (strcmp(req.path, "/health") == 0) {
            int booting = time(NULL) - started < boot_seconds;
            http_send_json(client, booting ? 503 : 200, booting ? "{\"status\":\"starting\"}" : "{\"status\":\"ok\"}");
        } else {
            if (slow_ms > 0) usleep((useconds_t)slow_ms * 1000);
            if (rand() % 100 < error_rate) {
                snprintf(body, sizeof body, "{\"error\":\"simulated bug\",\"version\":\"%s\",\"port\":%d}", version, port);
                http_send_json(client, 500, body);
            } else {
                snprintf(body, sizeof body, "{\"message\":\"Hello from %s\",\"version\":\"%s\",\"port\":%d}",
                         version, version, port);
                http_send_json(client, 200, body);
            }
            if (crash_after > 0 && ++served >= crash_after) {
                printf("%s: simulated crash after %ld requests\n", version, served);
                fflush(stdout);
                abort();  // dies with SIGABRT, like a real crash would
            }
        }
        close(client);
    }

    printf("%s on port %d: got SIGTERM, shutting down cleanly\n", version, port);
    close(listen_fd);
    return 0;
}
