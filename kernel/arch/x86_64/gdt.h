#ifndef KERNEL_ARCH_X86_64_GDT_H
#define KERNEL_ARCH_X86_64_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE  0x08U
#define GDT_KERNEL_DATA  0x10U
#define GDT_USER_DATA    0x2bU   /* index 5, RPL=3 */
#define GDT_USER_CODE_64 0x33U   /* index 6, RPL=3 */

void x86_64_gdt_init(void);
uint16_t gdt_install_tss(void *tss, uint32_t tss_size);

#endif

