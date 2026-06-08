#ifndef KERNEL_NET_SOCKET_H
#define KERNEL_NET_SOCKET_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_SOCKETS 8

/* Allocate a socket slot. Returns index [0, MAX_SOCKETS) or -1. */
int  sock_alloc(void);

/* Release a socket slot (does NOT close TCP conn — call sock_close). */
void sock_free(int idx);

/* Initiate a TCP connection. Returns 0 on success, -1 on failure. */
int  sock_connect(int idx, uint32_t ip, uint16_t port);

/* Send `len` bytes. Returns bytes sent or -1. */
int  sock_send(int idx, const void *buf, uint32_t len);

/* Receive up to `len` bytes. Returns bytes read, 0 = EOF, -1 = error. */
int  sock_recv(int idx, void *buf, uint32_t len);

/* Close TCP connection and free slot. */
void sock_close(int idx);

/* True if the socket has a live TCP connection. */
bool sock_is_connected(int idx);

#endif
