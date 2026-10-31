#ifndef INSTANCES_H
#define INSTANCES_H

#include "controller.h"
#include "../common/strbuf.h"

#include <pthread.h>
#include <sys/types.h>
#include <time.h>

#define MAX_INSTANCES 16

typedef enum {
    EMPTY,      // free slot
    STARTING,   // process started, waiting for its first good health check
    READY,      // healthy: the load balancer sends it traffic
    UNHEALTHY,  // failing health checks: no traffic until it recovers
    STOPPING,   // being shut down on purpose: no new traffic
    CRASHED,    // the process died unexpectedly: it will be restarted
} InstanceState;

// One running copy of the service. Slot i always uses port base_port + i.
typedef struct {
    int           port;
    pid_t         pid;               // 0 when no process is running
    char          version[VERSION_MAX + 1];
    InstanceState state;
    int           is_canary;
    int           restart_on_crash;  // 0 for a canary under test: a crash fails the deployment instead
    int           restarts;
    int           failed_checks;     // health checks failed in a row
    time_t        started_at;
    time_t        restart_at;        // when a CRASHED instance will be restarted (backoff)
    long          requests;          // traffic stats, counted by the load balancer
    long          errors;            // HTTP 5xx answers, or no answer at all
    int           in_flight;         // requests being served right now
} Instance;

// The instance table is shared by all threads: hold instances_lock while
// reading or changing it. instances_changed is broadcast after every change,
// so a thread can sleep until something happens ("the canary is ready").
extern Instance        instances[MAX_INSTANCES];
extern pthread_mutex_t instances_lock;
extern pthread_cond_t  instances_changed;

void        instances_init(void);
int         release_exists(const char *version);
const char *state_name(InstanceState state);

// Call these while holding instances_lock.
int instance_start(const char *version, int is_canary);  // returns the slot, or -1
int instances_count(const char *version);                // running or restarting (not stopping)
int instances_ready(const char *version);
int instances_wait(time_t deadline);                     // sleep until a change; 0 once past the deadline

// These take instances_lock themselves.
void instance_stop(int slot);  // graceful: drain, SIGTERM, then SIGKILL if needed
void instances_stop_all(void);
void instances_json(StrBuf *out);

void *supervisor_thread(void *arg);  // collects exited child processes and spots crashes
void *monitor_thread(void *arg);     // health checks, and restarts crashed instances

#endif
