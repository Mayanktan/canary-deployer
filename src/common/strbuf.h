#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>

// A string that grows as you append to it, so we never overflow or cut off
// a response because of a fixed-size buffer.
typedef struct {
    char  *data;   // always '\0'-terminated
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_append(StrBuf *sb, const char *data, size_t n);
void sb_printf(StrBuf *sb, const char *fmt, ...);

// Appends s as a quoted JSON string, escaping quotes, backslashes and control characters.
void sb_json_string(StrBuf *sb, const char *s);

#endif
