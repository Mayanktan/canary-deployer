#include "proxy.h"
#include "instances.h"
#include "thread_pool.h"
#include "../common/http.h"
#include "../common/net.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CLIENT_TIMEOUT_MS  5000
#define BACKEND_TIMEOUT_MS 5000
#define MAX_ATTEMPTS       3     // instances to try before giving up on a request

static int         listen_fd = -1;
static pthread_t   accept_tid;
static ThreadPool *pool;
static int         next_slot;  // round-robin position, protected by instances_lock

// Picks the next READY instance, taking turns. Returns the slot, or -1 if none is ready.
static int pick_instance(void) {
    int chosen = -1;
    pthread_mutex_lock(&instances_lock);
    for (int i = 0; i < MAX_INSTANCES; i++) {
        int slot = (next_slot + i) % MAX_INSTANCES;
        if (instances[slot].state == READY) {
            chosen = slot;
            next_slot = slot + 1;
            instances[slot].in_flight++;
            break;
        }
    }
    pthread_mutex_unlock(&instances_lock);
    return chosen;
}

static void record_result(int slot, int failed) {
    pthread_mutex_lock(&instances_lock);
    Instance *in = &instances[slot];
    in->requests++;
    if (failed) in->errors++;
    if (in->in_flight > 0) in->in_flight--;
    pthread_cond_broadcast(&instances_changed);  // instance_stop() may be waiting for in_flight to reach 0
    pthread_mutex_unlock(&instances_lock);
}

// Sends the request to the instance on `port` and streams its answer back to
// the client. Returns the instance's HTTP status, 0 if it accepted the request
// but never answered, or -1 if it could not be reached (safe to try another).
static int forward(int port, const HttpRequest *req, int client_fd) {
    int fd = net_connect("127.0.0.1", port);
    if (fd < 0) return -1;
    net_set_timeout(fd, BACKEND_TIMEOUT_MS);

    int status = 0;
    if (net_send_all(fd, req->raw, req->raw_len) == 0) {
        char chunk[4096];
        ssize_t n;
        while ((n = recv(fd, chunk, sizeof chunk - 1, 0)) > 0) {
            if (status == 0) {  // the first chunk starts with e.g. "HTTP/1.1 200 OK"
                chunk[n] = '\0';
                if (sscanf(chunk, "HTTP/%*s %d", &status) != 1) status = 502;
            }
            net_send_all(client_fd, chunk, (size_t)n);
        }
    }
    close(fd);
    return status;
}

static void handle_client(int client_fd) {
    net_set_timeout(client_fd, CLIENT_TIMEOUT_MS);
    HttpRequest req;
    int error = http_read_request(client_fd, &req);
    if (error) {
        http_send_json(client_fd, error, "{\"error\":\"bad request\"}");
        close(client_fd);
        return;
    }

    // If an instance can't be reached (say it just crashed), quietly try the
    // next one, so the user never sees that failure. A GET that got no answer
    // is retried too: GETs don't change anything, so sending one twice is safe.
    int is_get = strcmp(req.method, "GET") == 0;
    int status = -1;
    for (int attempt = 0; attempt < MAX_ATTEMPTS && (status == -1 || (status == 0 && is_get)); attempt++) {
        int slot = pick_instance();
        if (slot < 0) break;
        status = forward(config.base_port + slot, &req, client_fd);
        record_result(slot, status <= 0 || status >= 500);
    }

    if (status == -1) http_send_json(client_fd, 503, "{\"error\":\"no healthy instance available\"}");
    if (status == 0) http_send_json(client_fd, 504, "{\"error\":\"the instance did not answer in time\"}");
    close(client_fd);
}

static void *accept_loop(void *arg) {
    (void)arg;
    while (running) {
        int client_fd = net_accept(listen_fd, 500);
        if (client_fd >= 0) pool_submit(pool, client_fd);
    }
    return NULL;
}

int proxy_start(int worker_threads) {
    listen_fd = net_listen("0.0.0.0", config.proxy_port);
    if (listen_fd < 0) {
        fprintf(stderr, "load balancer: cannot listen on port %d: ", config.proxy_port);
        perror(NULL);
        return -1;
    }
    pool = pool_create(worker_threads, 256, handle_client);
    pthread_create(&accept_tid, NULL, accept_loop, NULL);
    return 0;
}

void proxy_stop(void) {
    if (listen_fd < 0) return;
    pthread_join(accept_tid, NULL);  // returns within 500 ms once running == 0
    pool_destroy(pool);              // finishes requests already queued
    close(listen_fd);
    listen_fd = -1;
}
