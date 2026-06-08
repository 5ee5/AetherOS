#include "net/socket.h"
#include "net/tcp.h"

static tcp_conn_t *s_socks[MAX_SOCKETS];

int sock_alloc(void)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!s_socks[i]) return i;
    }
    return -1;
}

void sock_free(int idx)
{
    if (idx < 0 || idx >= MAX_SOCKETS) return;
    s_socks[idx] = (tcp_conn_t *)0;
}

int sock_connect(int idx, uint32_t ip, uint16_t port)
{
    if (idx < 0 || idx >= MAX_SOCKETS) return -1;
    tcp_conn_t *c = tcp_connect(ip, port);
    if (!c) return -1;
    s_socks[idx] = c;
    return 0;
}

int sock_send(int idx, const void *buf, uint32_t len)
{
    if (idx < 0 || idx >= MAX_SOCKETS || !s_socks[idx]) return -1;
    if (len > 65535) len = 65535;
    if (!tcp_send(s_socks[idx], buf, (uint16_t)len)) return -1;
    return (int)len;
}

int sock_recv(int idx, void *buf, uint32_t len)
{
    if (idx < 0 || idx >= MAX_SOCKETS || !s_socks[idx]) return -1;
    if (len > 65535) len = 65535;
    uint16_t n = tcp_recv(s_socks[idx], buf, (uint16_t)len);
    return (int)n;
}

void sock_close(int idx)
{
    if (idx < 0 || idx >= MAX_SOCKETS) return;
    if (s_socks[idx]) {
        tcp_close(s_socks[idx]);
        s_socks[idx] = (tcp_conn_t *)0;
    }
}

bool sock_is_connected(int idx)
{
    if (idx < 0 || idx >= MAX_SOCKETS) return false;
    return s_socks[idx] != (tcp_conn_t *)0;
}
