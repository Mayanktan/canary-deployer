#include "json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_spaces(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

// p points at an opening quote. Returns a pointer just past the closing quote.
static const char *skip_string(const char *p) {
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;  // skip the escaped character
    }
    return *p == '"' ? p + 1 : NULL;
}

// Returns a pointer just past the value starting at p, or NULL if malformed.
static const char *skip_value(const char *p) {
    if (*p == '"') return skip_string(p);

    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                if (!(p = skip_string(p))) return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }

    // number, true, false or null
    const char *start = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p == start ? NULL : p;
}

// Decodes the string literal at p into out. Returns 0 if malformed or too long.
static int read_string(const char *p, char *out, size_t out_size) {
    size_t n = 0;
    for (p++; *p != '"'; p++) {
        char c = *p;
        if (c == '\0') return 0;
        if (c == '\\') {
            c = *++p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'u') {  // \uXXXX: keep plain ASCII, replace anything else with '?'
                char hex[5] = {0};
                for (int i = 0; i < 4; i++) {
                    if (!isxdigit((unsigned char)p[i + 1])) return 0;
                    hex[i] = p[i + 1];
                }
                long code = strtol(hex, NULL, 16);
                c = (code > 0 && code < 128) ? (char)code : '?';
                p += 4;
            } else if (c != '"' && c != '\\' && c != '/') {
                return 0;
            }
        }
        if (n + 1 >= out_size) return 0;
        out[n++] = c;
    }
    out[n] = '\0';
    return 1;
}

// Finds `key` at the top level of the object and returns a pointer to its value.
static const char *find_value(const char *json, const char *key) {
    if (!json) return NULL;
    const char *p = skip_spaces(json);
    if (*p != '{') return NULL;
    p = skip_spaces(p + 1);

    while (*p == '"') {
        char name[64];
        int match = read_string(p, name, sizeof name) && strcmp(name, key) == 0;
        if (!(p = skip_string(p))) return NULL;
        p = skip_spaces(p);
        if (*p != ':') return NULL;
        p = skip_spaces(p + 1);
        if (match) return p;

        if (!(p = skip_value(p))) return NULL;
        p = skip_spaces(p);
        if (*p != ',') return NULL;  // reached '}' without finding the key
        p = skip_spaces(p + 1);
    }
    return NULL;
}

int json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    const char *v = find_value(json, key);
    return v && *v == '"' && read_string(v, out, out_size);
}

int json_get_int(const char *json, const char *key, long *out) {
    const char *v = find_value(json, key);
    if (!v || !(*v == '-' || isdigit((unsigned char)*v))) return 0;
    *out = strtol(v, NULL, 10);
    return 1;
}

int json_get_bool(const char *json, const char *key, int *out) {
    const char *v = find_value(json, key);
    if (v && strncmp(v, "true", 4) == 0)  { *out = 1; return 1; }
    if (v && strncmp(v, "false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

// Copies the {...} or [...] starting at p into out.
static int copy_value(const char *p, char *out, size_t out_size) {
    const char *end = skip_value(p);
    if (!end || (size_t)(end - p) + 1 > out_size) return 0;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return 1;
}

int json_get_object(const char *json, const char *key, char *out, size_t out_size) {
    const char *v = find_value(json, key);
    return v && *v == '{' && copy_value(v, out, out_size);
}

const char *json_get_array(const char *json, const char *key) {
    const char *v = find_value(json, key);
    return (v && *v == '[') ? v : NULL;
}

int json_next_object(const char **cursor, char *out, size_t out_size) {
    const char *p = *cursor;
    if (!p) return 0;
    if (*p == '[') p++;  // first call: step inside the array
    p = skip_spaces(p);
    if (*p == ',') p = skip_spaces(p + 1);
    if (*p != '{' || !copy_value(p, out, out_size)) return 0;  // end of array
    *cursor = skip_value(p);
    return 1;
}
