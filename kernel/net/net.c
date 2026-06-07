#include "net/net.h"
#include "net/dhcp.h"
#include "net/eth.h"
#include "net/nic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "drivers/e1000.h"
#include "sched/sched.h"
#include "sched/thread.h"

static uint8_t s_rx_frame[1536];

void net_poll(void)
{
    nic_t *nic = nic_get();
    if (!nic) return;
    uint16_t n = nic->recv(nic, s_rx_frame, sizeof(s_rx_frame));
    if (n > 0) eth_recv(s_rx_frame, n);
}

static void net_thread_fn(void *arg)
{
    (void)arg;
    for (;;) {
        net_poll();
        thread_yield();
    }
}

bool net_init(void)
{
    if (!e1000_init()) {
        serial_write("net: no NIC found\n");
        return false;
    }

    serial_write("net: NIC initialised, running DHCP\n");

    bool dhcp_ok = dhcp_discover();
    if (!dhcp_ok) {
        serial_write("net: DHCP failed (continuing without IP)\n");
    }

    /* Start the network polling thread. */
    thread_create(net_thread_fn, NULL);

    serial_write("net: online\n");
    return true;
}
