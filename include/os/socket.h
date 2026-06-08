#ifndef OS_SOCKET_H
#define OS_SOCKET_H

#include <stdint.h>

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

struct sockaddr_in {
    uint16_t sin_family;   /* AF_INET */
    uint16_t sin_port;     /* big-endian port */
    uint32_t sin_addr;     /* big-endian IPv4 address */
    uint8_t  sin_zero[8];
};

#endif
