#ifndef EVENTS_H
#define EVENTS_H

#include "../common/strbuf.h"

// The controller's timeline ("canary started", "instance crashed", ...).
// Each event is printed, appended to data/events.log, and kept in memory so
// `deployctl status` can show the most recent ones.

void events_open(const char *path);
void events_close(void);
void log_event(const char *fmt, ...);
void events_json(StrBuf *out, int last_n);  // [{"time":"14:05:01","message":"..."}, ...]

#endif
