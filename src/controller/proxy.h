#ifndef PROXY_H
#define PROXY_H

// The load balancer. Users connect to config.proxy_port; each request is
// passed to one READY instance, taking turns (round-robin), and the answer is
// passed back. Along the way it counts requests and errors per instance,
// which is what the deployer uses to judge a canary.

int  proxy_start(int worker_threads);
void proxy_stop(void);

#endif
