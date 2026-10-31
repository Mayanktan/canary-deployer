// deployctl: command-line tool for the controller's deploy API.
// Every command is one HTTP call to the controller (see src/controller/api.h).

#include "../common/http.h"
#include "../common/json.h"
#include "../common/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define RED    "\033[31m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"
#define RESET  "\033[0m"

static const char *host = "127.0.0.1";
static int         port = 8081;

static void usage(void) {
    fprintf(stderr,
            "usage: deployctl [-H host] [-p port] <command>\n\n"
            "  status            instances, the current deployment and recent events\n"
            "  watch             status, refreshed every second (Ctrl+C to stop)\n"
            "  deploy VERSION    start a canary deployment of a release\n"
            "  rollback          deploy the previous version again\n"
            "  abort             stop the running deployment and roll back\n"
            "  history           past deployments\n"
            "  releases          versions you can deploy\n");
    exit(2);
}

// Calls the API. Exits with a clear message if the controller can't be reached.
static HttpResponse call(const char *method, const char *path, const char *body) {
    HttpResponse res = http_call(host, port, method, path, body, 5000);
    if (res.status == 0) {
        fprintf(stderr, RED "Cannot reach the controller at %s:%d. Is it running?" RESET "\n", host, port);
        exit(1);
    }
    return res;
}

// Prints the "message" (green) or "error" (red) of a reply; returns the exit code.
static int print_reply(HttpResponse res) {
    char text[512] = "";
    int ok = res.status < 400;
    json_get_string(res.body, ok ? "message" : "error", text, sizeof text);
    printf("%s%s" RESET "\n", ok ? GREEN : RED, text);
    http_free(&res);
    return ok ? 0 : 1;
}

static void format_duration(long seconds, char *out, size_t size) {
    if (seconds < 60)        snprintf(out, size, "%lds", seconds);
    else if (seconds < 3600) snprintf(out, size, "%ldm%02lds", seconds / 60, seconds % 60);
    else                     snprintf(out, size, "%ldh%02ldm", seconds / 3600, seconds % 3600 / 60);
}

// One deployment as a table row: when, id, change, result, how long, why.
static void print_deployment(const char *json) {
    long id = 0, started = 0, duration = 0;
    char from[40] = "", to[40] = "", phase[20] = "", reason[256] = "", change[90], took[48], when[20];
    json_get_int(json, "id", &id);
    json_get_int(json, "started", &started);
    json_get_int(json, "duration", &duration);
    json_get_string(json, "from", from, sizeof from);
    json_get_string(json, "to", to, sizeof to);
    json_get_string(json, "phase", phase, sizeof phase);
    json_get_string(json, "reason", reason, sizeof reason);

    time_t t = (time_t)started;
    strftime(when, sizeof when, "%b %d %H:%M", localtime(&t));
    snprintf(change, sizeof change, "%s -> %s", from, to);
    format_duration(duration, took, sizeof took);
    const char *color = strcmp(phase, "succeeded") == 0 ? GREEN : strcmp(phase, "rolled back") == 0 ? RED : YELLOW;
    printf("%-13s #%-3ld %-12s %s%-12s" RESET " %-7s %s\n", when, id, change, color, phase, took, reason);
}

static int show_status(void) {
    HttpResponse res = call("GET", "/status", NULL);
    char deploy[2048] = "", current[1024], stable[40] = "?", item[1024];

    json_get_object(res.body, "deploy", deploy, sizeof deploy);
    json_get_string(deploy, "stable", stable, sizeof stable);
    printf(BOLD "Stable version: %s" RESET "\n", stable);
    if (json_get_object(deploy, "current", current, sizeof current)) {
        printf("Latest deployment: ");
        print_deployment(current);
    }

    printf("\n" BOLD "%-6s %-7s %-8s %-10s %9s %8s %9s %8s" RESET "\n",
           "PORT", "PID", "VERSION", "STATE", "REQUESTS", "ERRORS", "RESTARTS", "UPTIME");
    const char *cursor = json_get_array(res.body, "instances");
    while (json_next_object(&cursor, item, sizeof item)) {
        long port_no = 0, pid = 0, requests = 0, errors = 0, restarts = 0, uptime = 0;
        int canary = 0;
        char version[40] = "", state[16] = "", error_pct[16], up[48];
        json_get_int(item, "port", &port_no);
        json_get_int(item, "pid", &pid);
        json_get_int(item, "requests", &requests);
        json_get_int(item, "errors", &errors);
        json_get_int(item, "restarts", &restarts);
        json_get_int(item, "uptime", &uptime);
        json_get_bool(item, "canary", &canary);
        json_get_string(item, "version", version, sizeof version);
        json_get_string(item, "state", state, sizeof state);

        snprintf(error_pct, sizeof error_pct, "%.1f%%", requests ? 100.0 * errors / requests : 0.0);
        format_duration(uptime, up, sizeof up);
        const char *color = strcmp(state, "ready") == 0 ? GREEN
                          : strcmp(state, "starting") == 0 || strcmp(state, "stopping") == 0 ? YELLOW : RED;
        printf("%-6ld %-7ld %-8s %s%-10s" RESET " %9ld %8s %9ld %8s%s\n", port_no, pid, version, color, state,
               requests, error_pct, restarts, up, canary ? YELLOW "  <- canary" RESET : "");
    }

    printf("\n" BOLD "Recent events" RESET "\n");
    cursor = json_get_array(res.body, "events");
    while (json_next_object(&cursor, item, sizeof item)) {
        char when[16] = "", message[256] = "";
        json_get_string(item, "time", when, sizeof when);
        json_get_string(item, "message", message, sizeof message);
        printf(DIM "%s" RESET "  %s\n", when, message);
    }
    http_free(&res);
    return 0;
}

static int show_history(void) {
    HttpResponse res = call("GET", "/history", NULL);
    printf(BOLD "%-13s %-4s %-12s %-12s %-7s %s" RESET "\n", "WHEN", "ID", "CHANGE", "RESULT", "TOOK", "REASON");
    const char *cursor = json_get_array(res.body, "deployments");
    char item[1024];
    int count = 0;
    while (json_next_object(&cursor, item, sizeof item)) {
        print_deployment(item);
        count++;
    }
    if (count == 0) printf("No deployments yet.\n");
    http_free(&res);
    return 0;
}

static int show_releases(void) {
    HttpResponse res = call("GET", "/releases", NULL);
    // The reply is {"releases":["v1","v2",...]}: print each quoted name.
    const char *p = json_get_array(res.body, "releases");
    while (p && (p = strchr(p, '"'))) {
        const char *end = strchr(p + 1, '"');
        if (!end) break;
        printf("%.*s\n", (int)(end - p - 1), p + 1);
        p = end + 1;
    }
    http_free(&res);
    return 0;
}

static int deploy(const char *version) {
    StrBuf body;
    sb_init(&body);
    sb_printf(&body, "{\"version\":");
    sb_json_string(&body, version);
    sb_printf(&body, "}");
    int code = print_reply(call("POST", "/deploy", body.data));
    sb_free(&body);
    if (code == 0) printf("Follow it with: deployctl watch\n");
    return code;
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "H:p:")) != -1) {
        if (opt == 'H') host = optarg;
        else if (opt == 'p') port = atoi(optarg);
        else usage();
    }
    if (optind >= argc) usage();
    const char *command = argv[optind];

    if (strcmp(command, "status") == 0) return show_status();
    if (strcmp(command, "history") == 0) return show_history();
    if (strcmp(command, "releases") == 0) return show_releases();
    if (strcmp(command, "rollback") == 0) return print_reply(call("POST", "/rollback", NULL));
    if (strcmp(command, "abort") == 0) return print_reply(call("POST", "/abort", NULL));
    if (strcmp(command, "deploy") == 0 && optind + 1 < argc) return deploy(argv[optind + 1]);
    if (strcmp(command, "watch") == 0) {
        for (;;) {
            printf("\033[2J\033[H");  // clear the screen
            show_status();
            printf(DIM "\nRefreshing every second. Ctrl+C to stop." RESET "\n");
            fflush(stdout);
            sleep(1);
        }
    }
    usage();
}
