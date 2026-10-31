#include "http.h"
#include "net.h"
#include "strbuf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

// recv() failed or the client hung up: a timeout becomes 408, anything else 400.
static int read_failed(ssize_t n) {
    return (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ? 408 : 400;
}

int http_read_request(int fd, HttpRequest *req) {
    req->raw_len = 0;
    req->body = "";

    // 1. Read until the empty line that ends the headers.
    char *headers_end = NULL;
    while (!headers_end) {
        if (req->raw_len == HTTP_MAX_REQUEST) return 413;
        ssize_t n = recv(fd, req->raw + req->raw_len, HTTP_MAX_REQUEST - req->raw_len, 0);
        if (n <= 0) return read_failed(n);
        req->raw_len += (size_t)n;
        req->raw[req->raw_len] = '\0';
        headers_end = strstr(req->raw, "\r\n\r\n");
    }

    // 2. The first line, e.g. "POST /deploy HTTP/1.1".
    if (sscanf(req->raw, "%7s %255s", req->method, req->path) != 2) return 400;

    // 3. Content-Length tells us how big the body is.
    long content_length = 0;
    for (char *line = strstr(req->raw, "\r\n") + 2; line < headers_end; line = strstr(line, "\r\n") + 2) {
        if (strncasecmp(line, "Content-Length:", 15) == 0) content_length = strtol(line + 15, NULL, 10);
    }
    size_t header_len = (size_t)(headers_end - req->raw) + 4;
    if (content_length < 0) return 400;
    if (header_len + (size_t)content_length > HTTP_MAX_REQUEST) return 413;

    // 4. Part of the body may have arrived with the headers; read the rest.
    size_t total = header_len + (size_t)content_length;
    while (req->raw_len < total) {
        ssize_t n = recv(fd, req->raw + req->raw_len, total - req->raw_len, 0);
        if (n <= 0) return read_failed(n);
        req->raw_len += (size_t)n;
    }
    req->raw_len = total;
    req->raw[total] = '\0';
    req->body = req->raw + header_len;
    return 0;
}

const char *http_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

void http_send_json(int fd, int status, const char *json) {
    // Headers and body go out in one send(). Two small writes can make TCP
    // hold back the second one (Nagle's algorithm) and add ~40 ms of delay.
    StrBuf res;
    sb_init(&res);
    sb_printf(&res,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %zu\r\n"
              "Connection: close\r\n"
              "\r\n%s",
              status, http_reason(status), strlen(json), json);
    net_send_all(fd, res.data, res.len);
    sb_free(&res);
}

HttpResponse http_call(const char *host, int port, const char *method, const char *path,
                       const char *json_body, int timeout_ms) {
    HttpResponse res = { 0, NULL };
    int fd = net_connect(host, port);
    if (fd >= 0) {
        net_set_timeout(fd, timeout_ms);

        const char *body = json_body ? json_body : "";
        StrBuf req;
        sb_init(&req);
        sb_printf(&req,
                  "%s %s HTTP/1.1\r\n"
                  "Host: %s:%d\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %zu\r\n"
                  "Connection: close\r\n"
                  "\r\n%s",
                  method, path, host, port, strlen(body), body);

        if (net_send_all(fd, req.data, req.len) == 0) {
            // The server closes the connection when it is done, so read until EOF.
            StrBuf reply;
            sb_init(&reply);
            char chunk[4096];
            ssize_t n;
            while ((n = recv(fd, chunk, sizeof chunk, 0)) > 0) sb_append(&reply, chunk, (size_t)n);

            char *body_start = strstr(reply.data, "\r\n\r\n");
            if (n == 0 && body_start && sscanf(reply.data, "HTTP/%*s %d", &res.status) == 1) {
                res.body = strdup(body_start + 4);
            }
            sb_free(&reply);
        }
        sb_free(&req);
        close(fd);
    }
    if (!res.body) {
        res.status = 0;
        res.body = strdup("");
    }
    return res;
}

void http_free(HttpResponse *res) {
    free(res->body);
    res->body = NULL;
}
