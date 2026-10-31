#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Makes sure there is room for `extra` more bytes plus the '\0'.
static void reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 256;
    while (cap < sb->len + extra + 1) cap *= 2;
    sb->data = realloc(sb->data, cap);
    if (!sb->data) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    sb->cap = cap;
}

void sb_init(StrBuf *sb) {
    sb->data = NULL;
    sb->len = sb->cap = 0;
    reserve(sb, 0);
    sb->data[0] = '\0';
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

void sb_append(StrBuf *sb, const char *data, size_t n) {
    reserve(sb, n);
    memcpy(sb->data + sb->len, data, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(NULL, 0, fmt, args);  // first pass only measures
    va_end(args);
    if (n < 0) return;

    reserve(sb, (size_t)n);
    va_start(args, fmt);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, args);
    va_end(args);
    sb->len += (size_t)n;
}

void sb_json_string(StrBuf *sb, const char *s) {
    sb_append(sb, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') sb_printf(sb, "\\%c", c);
        else if (c < 0x20)         sb_printf(sb, "\\u%04x", c);
        else                       sb_append(sb, s, 1);
    }
    sb_append(sb, "\"", 1);
}
