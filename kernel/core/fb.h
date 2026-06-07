#ifndef KERNEL_CORE_FB_H
#define KERNEL_CORE_FB_H

#include "os/bootinfo.h"

void fb_init(const struct os_framebuffer_info *info, uint64_t direct_map_base);
void fb_clear(uint32_t color);

#endif

