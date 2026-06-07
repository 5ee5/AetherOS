#include "net/tcp.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "net/nic.h"

#include <stddef.h>
#include <stdint.h>

#include "lib/string.h"
#include "mem/heap.h"

/* ---- TCP header ---------------------------------------------------------- */

#define TCP_FLAG_FIN 0x01U
#define TCP_FLAG_SYN 0x02U
#define TCP_FLAG_RST 0x04U
#define TCP_FLAG_PSH 0x08U
#define TCP_FLAG_ACK 0x10U

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;  /* high 4 bits = header length in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_hdr_t;

/* ---- Connection state ---------------------------------------------------- */

typedef enum {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_TIME_WAIT,
} tcp_state_t;

#define TCP_RXBUF 4096U
#define MAX_CONNS 4

struct tcp_conn {
    bool        in_use;
    tcp_state_t state;
    uint32_t    dst_ip;
    uint16_t    dst_port;
    uint16_t    src_port;
    uint32_t    snd_seq;   /* next sequence number to send */
    uint32_t    snd_una;   /* oldest unacked seq */
    uint32_t    rcv_nxt;   /* next expected receive seq */
    uint16_t    rcv_win;   /* window we advertise */
    /* RX ring buffer */
    uint8_t     rxbuf[TCP_RXBUF];
    uint32_t    rxbuf_head;
    uint32_t    rxbuf_tail;
};

static struct tcp_conn s_conns[MAX_CONNS];
static uint16_t        s_next_port = 49152;

static struct tcp_conn *alloc_conn(void)
{
    for (int i = 0; i < MAX_CONNS; ++i)
        if (!s_conns[i].in_use) { s_conns[i].in_use = true; return &s_conns[i]; }
    return NULL;
}

static void rxbuf_push(struct tcp_conn *c, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; ++i) {
        uint32_t next = (c->rxbuf_tail + 1) % TCP_RXBUF;
        if (next == c->rxbuf_head) break; /* full, drop */
        c->rxbuf[c->rxbuf_tail] = data[i];
        c->rxbuf_tail = next;
    }
}

static uint16_t rxbuf_pop(struct tcp_conn *c, uint8_t *buf, uint16_t max)
{
    uint16_t n = 0;
    while (n < max && c->rxbuf_head != c->rxbuf_tail) {
        buf[n++] = c->rxbuf[c->rxbuf_head];
        c->rxbuf_head = (c->rxbuf_head + 1) % TCP_RXBUF;
    }
    return n;
}

/* ---- Send a TCP segment -------------------------------------------------- */

static bool tcp_send_seg(struct tcp_conn *c, uint8_t flags,
                          const void *data, uint16_t dlen)
{
    static uint8_t buf[1460 + sizeof(tcp_hdr_t)];
    if (dlen + sizeof(tcp_hdr_t) > sizeof(buf)) return false;

    tcp_hdr_t *hdr = (tcp_hdr_t *)buf;
    hdr->src_port = htons(c->src_port);
    hdr->dst_port = htons(c->dst_port);
    hdr->seq      = htonl(c->snd_seq);
    hdr->ack      = (flags & TCP_FLAG_ACK) ? htonl(c->rcv_nxt) : 0;
    hdr->data_off = (uint8_t)(sizeof(tcp_hdr_t) / 4) << 4;
    hdr->flags    = flags;
    hdr->window   = htons(c->rcv_win);
    hdr->checksum = 0;
    hdr->urgent   = 0;
    if (dlen) memcpy(buf + sizeof(tcp_hdr_t), data, dlen);

    uint16_t total = (uint16_t)(sizeof(tcp_hdr_t) + dlen);
    uint16_t csum  = ip_pseudo_checksum(ipv4_our_ip(), c->dst_ip,
                                         IP_PROTO_TCP, buf, total);
    hdr->checksum  = htons(csum);

    if (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN))
        c->snd_seq++;
    else
        c->snd_seq += dlen;

    return ipv4_send(c->dst_ip, IP_PROTO_TCP, buf, total);
}

/* ---- Poll NIC for incoming frames ---------------------------------------- */

static uint8_t s_poll_buf[1536];

static void poll_once(void)
{
    nic_t *nic = nic_get();
    if (!nic) return;
    uint16_t n = nic->recv(nic, s_poll_buf, sizeof(s_poll_buf));
    if (n) eth_recv(s_poll_buf, n);
}

/* ---- Public API ---------------------------------------------------------- */

tcp_conn_t *tcp_connect(uint32_t dst_ip, uint16_t dst_port)
{
    struct tcp_conn *c = alloc_conn();
    if (!c) return NULL;

    c->state      = TCP_CLOSED;
    c->dst_ip     = dst_ip;
    c->dst_port   = dst_port;
    c->src_port   = s_next_port++;
    c->snd_seq    = 0x12345678U;
    c->snd_una    = c->snd_seq;
    c->rcv_nxt    = 0;
    c->rcv_win    = TCP_RXBUF - 1;
    c->rxbuf_head = 0;
    c->rxbuf_tail = 0;

    /* Send SYN. */
    c->state = TCP_SYN_SENT;
    tcp_send_seg(c, TCP_FLAG_SYN, NULL, 0);

    /* Wait for SYN-ACK (up to ~3s). */
    for (uint32_t i = 0; i < 300000U && c->state == TCP_SYN_SENT; ++i) {
        poll_once();
        for (volatile int j = 0; j < 100; ++j) {}
    }

    if (c->state != TCP_ESTABLISHED) {
        c->in_use = false;
        return NULL;
    }
    return c;
}

bool tcp_send(tcp_conn_t *c, const void *data, uint16_t len)
{
    if (!c || c->state != TCP_ESTABLISHED) return false;
    /* Chunk into MSS-sized segments. */
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        uint16_t chunk = (len > 1460) ? 1460 : len;
        if (!tcp_send_seg(c, TCP_FLAG_ACK | TCP_FLAG_PSH, p, chunk)) return false;
        p   += chunk;
        len -= chunk;
    }
    return true;
}

uint16_t tcp_recv(tcp_conn_t *c, void *buf, uint16_t max)
{
    if (!c || c->state != TCP_ESTABLISHED) return 0;
    return rxbuf_pop(c, (uint8_t *)buf, max);
}

void tcp_close(tcp_conn_t *c)
{
    if (!c) return;
    if (c->state == TCP_ESTABLISHED)
        tcp_send_seg(c, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
    c->state  = TCP_CLOSED;
    c->in_use = false;
}

/* ---- Incoming segment handler (called from ipv4.c) ----------------------- */

void tcp_input(uint32_t src_ip, uint32_t dst_ip, const void *pkt, uint16_t len)
{
    (void)dst_ip;
    if (len < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *hdr = (const tcp_hdr_t *)pkt;
    uint8_t doff   = (hdr->data_off >> 4) * 4U;
    if (doff < sizeof(tcp_hdr_t) || doff > len) return;
    const uint8_t *data = (const uint8_t *)pkt + doff;
    uint16_t dlen = (uint16_t)(len - doff);
    uint8_t  flags = hdr->flags;
    uint16_t sport = ntohs(hdr->src_port);
    uint16_t dport = ntohs(hdr->dst_port);

    /* Find matching connection. */
    struct tcp_conn *c = NULL;
    for (int i = 0; i < MAX_CONNS; ++i) {
        if (s_conns[i].in_use &&
            s_conns[i].dst_ip   == src_ip &&
            s_conns[i].dst_port == sport  &&
            s_conns[i].src_port == dport) {
            c = &s_conns[i];
            break;
        }
    }
    if (!c) return;

    uint32_t seq = ntohl(hdr->seq);
    uint32_t ack = ntohl(hdr->ack);

    switch (c->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->state   = TCP_ESTABLISHED;
            tcp_send_seg(c, TCP_FLAG_ACK, NULL, 0);
        } else if (flags & TCP_FLAG_RST) {
            c->state = TCP_CLOSED;
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_FLAG_RST) { c->state = TCP_CLOSED; break; }
        if (flags & TCP_FLAG_ACK) c->snd_una = ack;
        if (dlen > 0 && seq == c->rcv_nxt) {
            rxbuf_push(c, data, dlen);
            c->rcv_nxt += dlen;
            /* Send ACK. */
            tcp_send_seg(c, TCP_FLAG_ACK, NULL, 0);
        }
        if (flags & TCP_FLAG_FIN) {
            c->rcv_nxt++;
            c->state = TCP_CLOSE_WAIT;
            tcp_send_seg(c, TCP_FLAG_ACK, NULL, 0);
            tcp_send_seg(c, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            c->state = TCP_CLOSED;
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_FLAG_ACK) c->state = TCP_FIN_WAIT_2;
        /* fall through */
    case TCP_FIN_WAIT_2:
        if (flags & TCP_FLAG_FIN) {
            c->rcv_nxt++;
            tcp_send_seg(c, TCP_FLAG_ACK, NULL, 0);
            c->state = TCP_CLOSED;
        }
        break;

    default: break;
    }
}
