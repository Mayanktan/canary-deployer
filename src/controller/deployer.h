#ifndef DEPLOYER_H
#define DEPLOYER_H

#include "../common/strbuf.h"
#include <stddef.h>

// Runs deployments: canary first, then a rolling update, with an automatic
// rollback if anything goes wrong. One deployment at a time, each in its own thread.

int  deployer_init(const char *initial_version);  // loads saved state and starts the stable version
void deployer_shutdown(void);

// API actions. Each returns an HTTP status and writes a message for the user.
int deployer_deploy(const char *version, char *message, size_t size);
int deployer_rollback(char *message, size_t size);
int deployer_abort(char *message, size_t size);

void deployer_status_json(StrBuf *out);   // {"stable":"v2","previous":"v1","in_progress":false,"current":{...}}
void deployer_history_json(StrBuf *out);  // {"deployments":[...]}, newest first

#endif
