#include "api.h"
#include "deployer.h"
#include "events.h"
#include "instances.h"
#include "../common/http.h"
#include "../common/json.h"
#include "../common/net.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int       listen_fd = -1;
static pthread_t api_tid;

// {"message": "..."} for success, {"error": "..."} for failure.
static int reply(StrBuf *out, int status, const char *text) {
    sb_printf(out, "{\"%s\":", status < 400 ? "message" : "error");
    sb_json_string(out, text);
    sb_printf(out, "}");
    return status;
}

static int is_release(const struct dirent *entry) {
    size_t len = strlen(entry->d_name);
    return len > 3 && strcmp(entry->d_name + len - 3, ".sh") == 0;
}

// Every <name>.sh in the releases folder is a version that can be deployed.
static int list_releases(StrBuf *out) {
    struct dirent **entries;
    int n = scandir(config.releases_dir, &entries, is_release, alphasort);
    sb_printf(out, "{\"releases\":[");
    for (int i = 0; i < n; i++) {
        entries[i]->d_name[strlen(entries[i]->d_name) - 3] = '\0';  // drop ".sh"
        if (i) sb_printf(out, ",");
        sb_json_string(out, entries[i]->d_name);
        free(entries[i]);
    }
    if (n >= 0) free(entries);
    sb_printf(out, "]}");
    return 200;
}

static int route(const HttpRequest *req, StrBuf *out) {
    int is_get = strcmp(req->method, "GET") == 0;
    int is_post = strcmp(req->method, "POST") == 0;
    char message[256];
    int status;

    if (is_get && strcmp(req->path, "/status") == 0) {
        sb_printf(out, "{\"deploy\":");
        deployer_status_json(out);
        sb_printf(out, ",\"instances\":");
        instances_json(out);
        sb_printf(out, ",\"events\":");
        events_json(out, 12);
        sb_printf(out, "}");
        return 200;
    }
    if (is_get && strcmp(req->path, "/history") == 0) {
        deployer_history_json(out);
        return 200;
    }
    if (is_get && strcmp(req->path, "/releases") == 0) {
        return list_releases(out);
    }
    if (is_post && strcmp(req->path, "/deploy") == 0) {
        char version[VERSION_MAX + 1];
        if (!json_get_string(req->body, "version", version, sizeof version)) {
            return reply(out, 400, "send JSON like {\"version\": \"v2\"}");
        }
        status = deployer_deploy(version, message, sizeof message);
        return reply(out, status, message);
    }
    if (is_post && strcmp(req->path, "/rollback") == 0) {
        status = deployer_rollback(message, sizeof message);
        return reply(out, status, message);
    }
    if (is_post && strcmp(req->path, "/abort") == 0) {
        status = deployer_abort(message, sizeof message);
        return reply(out, status, message);
    }
    return reply(out, 404, "no such endpoint");
}

// API requests are rare and quick, so one thread serves them one at a time.
static void *api_loop(void *arg) {
    (void)arg;
    while (running) {
        int client_fd = net_accept(listen_fd, 500);
        if (client_fd < 0) continue;
        net_set_timeout(client_fd, 5000);

        HttpRequest req;
        StrBuf out;
        sb_init(&out);
        int status = http_read_request(client_fd, &req);
        status = status ? reply(&out, status, http_reason(status)) : route(&req, &out);
        http_send_json(client_fd, status, out.data);
        sb_free(&out);
        close(client_fd);
    }
    return NULL;
}

int api_start(void) {
    listen_fd = net_listen("127.0.0.1", config.api_port);
    if (listen_fd < 0) {
        fprintf(stderr, "deploy API: cannot listen on port %d: ", config.api_port);
        perror(NULL);
        return -1;
    }
    pthread_create(&api_tid, NULL, api_loop, NULL);
    return 0;
}

void api_stop(void) {
    if (listen_fd < 0) return;
    pthread_join(api_tid, NULL);
    close(listen_fd);
    listen_fd = -1;
}
