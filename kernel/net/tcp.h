#ifndef KERNEL_NET_TCP_H
#define KERNEL_NET_TCP_H

#include <stdbool.h>
#include <stdint.h>

/* Opaque TCP connection handle. */
typedef struct tcp_conn tcp_conn_t;

/* Active open (client).  Returns NULL on failure.
   Blocks (polling) until connection established or timeout.  */
tcp_conn_t *tcp_connect(uint32_t dst_ip, uint16_t dst_port);

/* Send data.  Blocks until sent (polling). */
bool tcp_send(tcp_conn_t *c, const void *data, uint16_t len);

/* Receive up to `max` bytes.  Returns 0 if nothing available. */
uint16_t tcp_recv(tcp_conn_t *c, void *buf, uint16_t max);

/* Close connection (sends FIN). */
void tcp_close(tcp_conn_t *c);

/* Returns true if the connection is in ESTABLISHED state. */
bool tcp_is_established(tcp_conn_t *c);

/* Called from ipv4.c. */
void tcp_input(uint32_t src_ip, uint32_t dst_ip, const void *pkt, uint16_t len);

#endif
