#ifndef JSON_H
#define JSON_H

#include <stddef.h>

// A deliberately small JSON reader. It handles what this project's API sends:
// objects whose values are strings, integers, booleans, nested objects or
// arrays of objects. It is not a general-purpose JSON library.
//
// Every lookup searches the top level of the object in `json` and returns
// 1 if `key` was found with the expected type, 0 otherwise.

int json_get_string(const char *json, const char *key, char *out, size_t out_size);
int json_get_int(const char *json, const char *key, long *out);
int json_get_bool(const char *json, const char *key, int *out);

// Copies a nested {...} object, e.g. to then read fields from it.
int json_get_object(const char *json, const char *key, char *out, size_t out_size);

// Iterating over an array of objects:
//
//     const char *cursor = json_get_array(body, "instances");
//     char item[1024];
//     while (json_next_object(&cursor, item, sizeof item)) {
//         json_get_int(item, "port", &port);
//     }
const char *json_get_array(const char *json, const char *key);
int         json_next_object(const char **cursor, char *out, size_t out_size);

#endif
