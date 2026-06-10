#include "net/socket.h"
#include "net/tcp.h"

#include <stdint.h>

#include "sync/spinlock.h"

static tcp_conn_t *s_socks[MAX_SOCKETS];
/* Reservation flags: a slot is busy from sock_alloc() until sock_free()/close,
   even before sock_connect() installs the tcp_conn_t. Without this, two
   concurrent sock_alloc() calls could hand out the same index. */
static bool        s_used[MAX_SOCKETS];
static spinlock_t  sock_lock = SPINLOCK_INIT;

int sock_alloc(void)
{
    spinlock_acquire(&sock_lock);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!s_used[i]) {
            s_used[i]  = true;
            s_socks[i] = (tcp_conn_t *)0;
            spinlock_release(&sock_lock);
            return i;
        }
    }
    spinlock_release(&sock_lock);
    return -1;
}

void sock_free(int idx)
{
    if (idx < 0 || idx >= MAX_SOCKETS) return;
    spinlock_acquire(&sock_lock);
    s_socks[idx] = (tcp_conn_t *)0;
    s_used[idx]  = false;
    spinlock_release(&sock_lock);
}

int sock_connect(int idx, uint32_t ip, uint16_t port)
{
    if (idx < 0 || idx >= MAX_SOCKETS) return -1;
    tcp_conn_t *c = tcp_connect(ip, port);
    if (!c) return -1;
    spinlock_acquire(&sock_lock);
    s_socks[idx] = c;
    spinlock_release(&sock_lock);
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
    if (n == 0 && !tcp_is_established(s_socks[idx])) return -1; /* EOF */
    return (int)n;
}

void sock_close(int idx)
{
    if (idx < 0 || idx >= MAX_SOCKETS) return;
    spinlock_acquire(&sock_lock);
    tcp_conn_t *c = s_socks[idx];
    s_socks[idx] = (tcp_conn_t *)0;
    s_used[idx]  = false;
    spinlock_release(&sock_lock);
    if (c) tcp_close(c);
}

bool sock_is_connected(int idx)
{
    if (idx < 0 || idx >= MAX_SOCKETS) return false;
    return s_socks[idx] != (tcp_conn_t *)0;
}
