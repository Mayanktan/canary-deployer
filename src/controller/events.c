#include "events.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#define MAX_EVENTS 50

// The last MAX_EVENTS events, in a ring buffer: when it is full, the newest
// event overwrites the oldest one.
static char messages[MAX_EVENTS][200];
static char clock_times[MAX_EVENTS][9];
static int  next_slot = 0;
static int  event_count = 0;

static FILE           *log_file;
static pthread_mutex_t events_lock = PTHREAD_MUTEX_INITIALIZER;

void events_open(const char *path) {
    log_file = fopen(path, "a");
    if (!log_file) perror(path);
}

void events_close(void) {
    if (log_file) fclose(log_file);
    log_file = NULL;
}

void log_event(const char *fmt, ...) {
    char message[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char date[32];
    strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &local);

    pthread_mutex_lock(&events_lock);
    snprintf(messages[next_slot], sizeof messages[next_slot], "%s", message);
    strftime(clock_times[next_slot], sizeof clock_times[next_slot], "%H:%M:%S", &local);
    next_slot = (next_slot + 1) % MAX_EVENTS;
    if (event_count < MAX_EVENTS) event_count++;

    printf("%s  %s\n", date + 11, message);
    fflush(stdout);
    if (log_file) {
        fprintf(log_file, "%s  %s\n", date, message);
        fflush(log_file);
    }
    pthread_mutex_unlock(&events_lock);
}

void events_json(StrBuf *out, int last_n) {
    pthread_mutex_lock(&events_lock);
    int n = last_n < event_count ? last_n : event_count;
    sb_printf(out, "[");
    for (int i = 0; i < n; i++) {
        int slot = (next_slot - n + i + MAX_EVENTS) % MAX_EVENTS;  // oldest first
        sb_printf(out, "%s{\"time\":\"%s\",\"message\":", i ? "," : "", clock_times[slot]);
        sb_json_string(out, messages[slot]);
        sb_printf(out, "}");
    }
    sb_printf(out, "]");
    pthread_mutex_unlock(&events_lock);
}
