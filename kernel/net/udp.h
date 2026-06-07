#ifndef KERNEL_NET_UDP_H
#define KERNEL_NET_UDP_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*udp_recv_fn)(uint32_t src_ip, uint16_t src_port,
                             const void *data, uint16_t len, void *ctx);

/* Bind a callback for incoming UDP on `port` (host byte order). */
void udp_bind(uint16_t port, udp_recv_fn fn, void *ctx);
void udp_unbind(uint16_t port);

/* Send a UDP datagram. */
bool udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t len);

/* Called from ipv4.c. */
void udp_recv(uint32_t src_ip, uint32_t dst_ip, const void *pkt, uint16_t len);

#endif
