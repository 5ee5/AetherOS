#ifndef KERNEL_NET_NIC_H
#define KERNEL_NET_NIC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct nic {
    uint8_t  mac[6];
    bool     (*send)(struct nic *self, const void *data, uint16_t len);
    uint16_t (*recv)(struct nic *self, void *buf, uint16_t max_len);
} nic_t;

void   nic_register(nic_t *nic);
nic_t *nic_get(void);

#endif
