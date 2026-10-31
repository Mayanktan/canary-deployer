#ifndef NET_H
#define NET_H

#include <stddef.h>

// Opens a TCP socket listening on `port`. Use "0.0.0.0" to accept connections
// from other machines, or "127.0.0.1" to only allow this machine.
// Returns the socket, or -1 on error (errno is set).
int net_listen(const char *ip, int port);

// Connects to host:port. Returns the socket, or -1 on error.
int net_connect(const char *host, int port);

// send() may write only part of the data; this keeps going until all of it is sent.
int net_send_all(int fd, const char *data, size_t len);

// Makes recv()/send() on this socket give up after `ms` milliseconds.
void net_set_timeout(int fd, int ms);

// Waits up to `ms` milliseconds for a connection on a listening socket.
// Returns the new client socket, or -1 if none arrived (or on error).
// Waiting in small steps lets the accepting thread notice a shutdown request.
int net_accept(int listen_fd, int ms);

#endif
