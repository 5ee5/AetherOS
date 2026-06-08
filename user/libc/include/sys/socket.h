#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <stdint.h>

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
};

static inline uint16_t htons(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0x000000FFU) << 24) |
           ((x & 0x0000FF00U) <<  8) |
           ((x & 0x00FF0000U) >>  8) |
           ((x & 0xFF000000U) >> 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

int  socket(int domain, int type, int proto);
int  connect(int fd, const struct sockaddr_in *addr, int addrlen);
long send(int fd, const void *buf, long len, int flags);
long recv(int fd, void *buf, long len, int flags);
int  dns_resolve(const char *hostname, uint32_t *ip_out);

#endif
