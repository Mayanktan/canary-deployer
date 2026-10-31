#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <stdatomic.h>

#define VERSION_MAX 32

// Settings from the command line. main.c fills them in at startup; after
// that every thread only reads them, so they need no lock.
typedef struct {
    int  instance_count;  // how many copies of the service to run
    int  proxy_port;      // users send traffic here (the load balancer)
    int  api_port;        // deployctl talks to this
    int  base_port;       // instances listen on base_port, base_port + 1, ...
    int  canary_seconds;  // how long to watch a canary before trusting it
    int  max_error_pct;   // a canary failing more requests than this is rolled back
    int  min_requests;    // a canary needs this many requests before we judge its error rate
    char releases_dir[256];
    char data_dir[256];
} Config;

extern Config     config;
extern atomic_int running;  // becomes 0 when the controller is shutting down

#endif
