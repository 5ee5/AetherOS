#include "drivers/usb_hid.h"
#include "drivers/xhci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/input.h"
#include "core/serial.h"
#include "lib/string.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "sched/thread.h"

/* ============================================================================
 * USB HID boot-protocol keyboard. Polls the interrupt-IN endpoint for 8-byte
 * boot reports [modifiers, reserved, key0..key5] and feeds decoded ASCII into
 * the shared console input ring. Emits on key-down transitions only.
 * ========================================================================== */

/* HID usage id -> ASCII (US layout), unshifted / shifted. Index = usage code. */
static const char s_hid_ascii[128] = {
    /*00*/ 0,0,0,0, 'a','b','c','d','e','f','g','h','i','j','k','l',
    /*10*/ 'm','n','o','p','q','r','s','t','u','v','w','x','y','z','1','2',
    /*20*/ '3','4','5','6','7','8','9','0','\n', 27, 8, '\t',' ','-','=','[',
    /*30*/ ']','\\', 0, ';','\'','`',',','.','/', 0, 0,0,0,0,0,0,
    /*40*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /*50*/ 0,0,0,0,'/','*','-','+','\n','1','2','3','4','5','6','7',
    /*60*/ '8','9','0','.', 0,0,0,0,0,0,0,0,0,0,0,0,
    /*70*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static const char s_hid_ascii_shift[128] = {
    /*00*/ 0,0,0,0, 'A','B','C','D','E','F','G','H','I','J','K','L',
    /*10*/ 'M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','!','@',
    /*20*/ '#','$','%','^','&','*','(',')','\n', 27, 8, '\t',' ','_','+','{',
    /*30*/ '}','|', 0, ':','"','~','<','>','?', 0, 0,0,0,0,0,0,
    /*40*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /*50*/ 0,0,0,0,'/','*','-','+','\n','1','2','3','4','5','6','7',
    /*60*/ '8','9','0','.', 0,0,0,0,0,0,0,0,0,0,0,0,
    /*70*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

#define HID_KEY_CAPSLOCK 0x39u

static int      s_dev = -1;
static uint8_t *s_report;      /* 8-byte DMA buffer */
static uint64_t s_report_phys;
static uint8_t  s_prev[8];
static bool     s_caps;

/* Was `key` present in the previous report? (for key-down edge detection) */
static bool was_down(uint8_t key)
{
    for (int i = 2; i < 8; ++i) if (s_prev[i] == key) return true;
    return false;
}

static void process_report(const uint8_t *rep)
{
    uint8_t mods = rep[0];
    bool shift = (mods & 0x22u) != 0;   /* left/right shift */

    for (int i = 2; i < 8; ++i) {
        uint8_t key = rep[i];
        if (key == 0 || key == 1 /* rollover */) continue;
        if (was_down(key)) continue;    /* only on new press */

        if (key == HID_KEY_CAPSLOCK) { s_caps = !s_caps; continue; }
        if (key >= 128) continue;

        char c = shift ? s_hid_ascii_shift[key] : s_hid_ascii[key];
        if (c >= 'a' && c <= 'z' && s_caps) c = (char)(c - 32);
        else if (c >= 'A' && c <= 'Z' && s_caps && !shift) c = (char)(c + 32);
        if (c) input_push(c);
    }
    memcpy(s_prev, rep, 8);
}

static void hid_poll_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int n = xhci_intr_poll(s_dev, s_report_phys, 8);
        if (n >= 8) process_report(s_report);
        thread_yield();
    }
}

bool usb_hid_init(void)
{
    xhci_init();   /* idempotent; usb_init likely ran it already */
    s_dev = xhci_find_device(0x03, 0x01, 0x01);   /* HID, boot, keyboard */
    if (s_dev < 0) return false;

    uint64_t rp = pmm_alloc_page();
    if (rp == PMM_ALLOC_FAILED) return false;
    s_report = (uint8_t *)(uintptr_t)(vmm_direct_map_base() + rp);
    s_report_phys = rp;
    memset(s_report, 0, 8);

    /* SET_PROTOCOL(boot=0) and SET_IDLE(0) on the interface (class requests). */
    xhci_control(s_dev, 0x21, 0x0B, 0x0000, 0, 0, 0);   /* SET_PROTOCOL boot */
    xhci_control(s_dev, 0x21, 0x0A, 0x0000, 0, 0, 0);   /* SET_IDLE infinite */

    thread_create(hid_poll_thread, NULL);
    serial_write("usb-hid: keyboard ready\n");
    return true;
}
