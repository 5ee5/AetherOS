#include "net/nic.h"
#include <stddef.h>

static nic_t *s_nic;

void nic_register(nic_t *nic) { s_nic = nic; }
nic_t *nic_get(void)          { return s_nic; }
