#ifndef KERNEL_ARCH_X86_64_TSS_H
#define KERNEL_ARCH_X86_64_TSS_H

#include <stdint.h>

struct tss64 {
	uint32_t reserved0;
	uint64_t rsp[3];
	uint64_t reserved1;
	uint64_t ist[7];
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iopb;
} __attribute__((packed));

void tss_init(void);
void tss_set_rsp0(uint64_t rsp0);

#endif
