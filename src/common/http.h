#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

// Just enough HTTP/1.1 for this project: one request per connection, and the
// server closes the connection after the response ("Connection: close").

#define HTTP_MAX_REQUEST 16384

// ---- Server side ----

typedef struct {
    char        raw[HTTP_MAX_REQUEST + 1];  // the request exactly as received (the load balancer forwards this)
    size_t      raw_len;
    char        method[8];                  // GET, POST, ...
    char        path[256];                  // e.g. /deploy
    const char *body;                       // points into raw; "" when there is no body
} HttpRequest;

// Reads one full request (headers + body). Returns 0, or an HTTP error code
// if the request is unusable: 400 malformed, 408 too slow, 413 too large.
int http_read_request(int fd, HttpRequest *req);

// Sends a complete response with a JSON body.
void http_send_json(int fd, int status, const char *json);

const char *http_reason(int status);  // 200 -> "OK", 404 -> "Not Found", ...

// ---- Client side ----

typedef struct {
    int   status;  // HTTP status code, or 0 if there was no answer
    char *body;    // never NULL; free with http_free()
} HttpResponse;

// Sends one request and waits up to timeout_ms for the full response.
// json_body may be NULL.
HttpResponse http_call(const char *host, int port, const char *method, const char *path,
                       const char *json_body, int timeout_ms);
void http_free(HttpResponse *res);

#endif
