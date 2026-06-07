#include "core/fb.h"

#include <stdint.h>

static volatile uint32_t *fb_base;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_stride;

void fb_init(const struct os_framebuffer_info *info, uint64_t direct_map_base)
{
	if (info->base == 0 || info->width == 0 || info->height == 0 ||
		info->pixels_per_scanline == 0) {
		return;
	}

	fb_base = (volatile uint32_t *)(uintptr_t)(direct_map_base + info->base);
	fb_width = info->width;
	fb_height = info->height;
	fb_stride = info->pixels_per_scanline;
}

void fb_clear(uint32_t color)
{
	if (fb_base == 0) {
		return;
	}

	for (uint32_t y = 0; y < fb_height; ++y) {
		for (uint32_t x = 0; x < fb_width; ++x) {
			fb_base[(uint64_t)y * fb_stride + x] = color;
		}
	}
}

volatile uint32_t *fb_get_base(void)   { return fb_base; }
uint32_t           fb_get_width(void)  { return fb_width; }
uint32_t           fb_get_height(void) { return fb_height; }
uint32_t           fb_get_stride(void) { return fb_stride; }

