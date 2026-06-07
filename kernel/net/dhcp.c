#include "net/dhcp.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "net/udp.h"
#include "net/nic.h"

#include <stdbool.h>
#include <stdint.h>

#include "core/serial.h"
#include "lib/string.h"
#include "mem/pmm.h"
#include "mem/vmm.h"

/* ---- DHCP message types -------------------------------------------------- */
#define DHCP_DISCOVER  1U
#define DHCP_OFFER     2U
#define DHCP_REQUEST   3U
#define DHCP_ACK       5U
#define DHCP_PORT_SERVER 67U
#define DHCP_PORT_CLIENT 68U

typedef struct {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[308];
} __attribute__((packed)) dhcp_msg_t;

#define DHCP_MAGIC 0x63825363U

static volatile bool s_got_offer;
static volatile bool s_got_ack;
static uint32_t      s_offered_ip;
static uint32_t      s_server_ip;
static uint32_t      s_subnet;
static uint32_t      s_gateway;
static uint32_t      s_dns;
static uint32_t      s_xid;

static void write_opt(uint8_t **p, uint8_t code, uint8_t len, const uint8_t *data)
{
    *(*p)++ = code;
    *(*p)++ = len;
    memcpy(*p, data, len);
    *p += len;
}

static void build_discover(dhcp_msg_t *m, const uint8_t mac[6])
{
    memset(m, 0, sizeof(*m));
    m->op    = 1; /* BOOTREQUEST */
    m->htype = 1; /* Ethernet */
    m->hlen  = 6;
    m->xid   = s_xid;
    m->flags = htons(0x8000U); /* broadcast */
    memcpy(m->chaddr, mac, 6);
    m->magic = htonl(DHCP_MAGIC);

    uint8_t *p = m->options;
    uint8_t msg = DHCP_DISCOVER;
    write_opt(&p, 53, 1, &msg);
    uint8_t req[] = { 1, 3, 6, 15 }; /* subnet, router, dns, domain */
    write_opt(&p, 55, sizeof(req), req);
    *p++ = 255; /* End */
}

static void build_request(dhcp_msg_t *m, const uint8_t mac[6],
                           uint32_t offered, uint32_t server)
{
    memset(m, 0, sizeof(*m));
    m->op    = 1;
    m->htype = 1;
    m->hlen  = 6;
    m->xid   = s_xid;
    m->flags = htons(0x8000U);
    memcpy(m->chaddr, mac, 6);
    m->magic = htonl(DHCP_MAGIC);

    uint8_t *p = m->options;
    uint8_t msg = DHCP_REQUEST;
    write_opt(&p, 53, 1, &msg);
    write_opt(&p, 50, 4, (const uint8_t *)&offered);
    write_opt(&p, 54, 4, (const uint8_t *)&server);
    *p++ = 255;
}

static void parse_options(const dhcp_msg_t *m, uint8_t expected_type)
{
    const uint8_t *p   = m->options;
    const uint8_t *end = m->options + sizeof(m->options);
    uint8_t msg_type = 0;
    uint32_t subnet = 0, router = 0, dns = 0;

    while (p < end && *p != 255) {
        uint8_t code = *p++;
        if (code == 0) continue;
        if (p >= end) break;
        uint8_t len = *p++;
        if (p + len > end) break;
        switch (code) {
        case 53: msg_type = p[0]; break;
        case 1:  if (len >= 4) memcpy(&subnet, p, 4); break;
        case 3:  if (len >= 4) memcpy(&router, p, 4); break;
        case 6:  if (len >= 4) memcpy(&dns,    p, 4); break;
        case 54: if (len >= 4) memcpy(&s_server_ip, p, 4); break;
        }
        p += len;
    }

    if (msg_type == DHCP_OFFER && expected_type == DHCP_OFFER) {
        s_offered_ip = m->yiaddr;
        s_subnet     = subnet;
        s_gateway    = router;
        s_dns        = dns;
        s_got_offer  = true;
    } else if (msg_type == DHCP_ACK && expected_type == DHCP_ACK) {
        s_offered_ip = m->yiaddr;
        if (subnet)  s_subnet  = subnet;
        if (router)  s_gateway = router;
        if (dns)     s_dns     = dns;
        s_got_ack    = true;
    }
}

static void dhcp_recv_offer(uint32_t src_ip, uint16_t src_port,
                             const void *data, uint16_t len, void *ctx)
{
    (void)src_ip; (void)src_port; (void)ctx;
    if (len < sizeof(dhcp_msg_t)) return;
    const dhcp_msg_t *m = (const dhcp_msg_t *)data;
    if (m->xid != s_xid) return;
    if (ntohl(m->magic) != DHCP_MAGIC) return;
    parse_options(m, DHCP_OFFER);
}

static void dhcp_recv_ack(uint32_t src_ip, uint16_t src_port,
                           const void *data, uint16_t len, void *ctx)
{
    (void)src_ip; (void)src_port; (void)ctx;
    if (len < sizeof(dhcp_msg_t)) return;
    const dhcp_msg_t *m = (const dhcp_msg_t *)data;
    if (m->xid != s_xid) return;
    if (ntohl(m->magic) != DHCP_MAGIC) return;
    parse_options(m, DHCP_ACK);
}

/* Poll the NIC and dispatch incoming frames until `done` is set or timeout. */
static void poll_until(volatile bool *done, uint32_t iters)
{
    static uint8_t pkt[1536];
    nic_t *nic = nic_get();
    if (!nic) return;
    for (uint32_t i = 0; i < iters && !(*done); ++i) {
        uint16_t n = nic->recv(nic, pkt, sizeof(pkt));
        if (n > 0) eth_recv(pkt, n);
        for (volatile int j = 0; j < 5000; ++j) {}
    }
}

bool dhcp_discover(void)
{
    nic_t *nic = nic_get();
    if (!nic) return false;

    s_xid = 0xdeadbe01U;
    s_got_offer = false;
    s_got_ack   = false;

    udp_bind(DHCP_PORT_CLIENT, dhcp_recv_offer, NULL);

    dhcp_msg_t msg;
    uint32_t broadcast = 0xffffffffU;

    /* DISCOVER */
    build_discover(&msg, nic->mac);
    /* Send from 0.0.0.0 before we have an IP.
       Temporarily set IP to 0 for the send. */
    ipv4_set_config(0, 0, 0, 0);
    udp_send(broadcast, DHCP_PORT_CLIENT, DHCP_PORT_SERVER, &msg, sizeof(msg));

    /* Wait for OFFER (up to ~2s). */
    poll_until(&s_got_offer, 400000);

    if (!s_got_offer) {
        serial_write("dhcp: no offer received\n");
        udp_unbind(DHCP_PORT_CLIENT);
        return false;
    }
    serial_write("dhcp: got offer\n");

    udp_unbind(DHCP_PORT_CLIENT);
    udp_bind(DHCP_PORT_CLIENT, dhcp_recv_ack, NULL);

    /* REQUEST */
    build_request(&msg, nic->mac, s_offered_ip, s_server_ip);
    ipv4_set_config(0, 0, 0, 0);
    udp_send(broadcast, DHCP_PORT_CLIENT, DHCP_PORT_SERVER, &msg, sizeof(msg));

    /* Wait for ACK (up to ~2s). */
    poll_until(&s_got_ack, 400000);
    udp_unbind(DHCP_PORT_CLIENT);

    if (!s_got_ack) {
        serial_write("dhcp: no ack received\n");
        return false;
    }

    ipv4_set_config(s_offered_ip, s_subnet, s_gateway, s_dns);

    serial_write("dhcp: IP=");
    const uint8_t *ip = (const uint8_t *)&s_offered_ip;
    serial_write_dec(ip[0]); serial_write(".");
    serial_write_dec(ip[1]); serial_write(".");
    serial_write_dec(ip[2]); serial_write(".");
    serial_write_dec(ip[3]); serial_write("\n");
    return true;
}
