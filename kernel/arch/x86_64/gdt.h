#ifndef KERNEL_ARCH_X86_64_GDT_H
#define KERNEL_ARCH_X86_64_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE  0x08U
#define GDT_KERNEL_DATA  0x10U
#define GDT_USER_DATA    0x2bU   /* index 5, RPL=3 */
#define GDT_USER_CODE_64 0x33U   /* index 6, RPL=3 */

/* Builds and loads the BSP's GDT (slot 0). */
void x86_64_gdt_init(void);

/* Builds and loads the GDT for one CPU.  Every CPU needs its own because the
   TSS descriptor differs; all other entries are identical, so the selectors
   above are valid everywhere. */
void x86_64_gdt_init_cpu(uint32_t slot);

/* Writes the 64-bit TSS descriptor into `slot`'s GDT and returns its selector. */
uint16_t gdt_install_tss(uint32_t slot, void *tss, uint32_t tss_size);

#endif

