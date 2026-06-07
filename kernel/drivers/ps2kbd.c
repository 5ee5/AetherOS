#include "drivers/ps2kbd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/idt.h"
#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/apic.h"
#include "core/serial.h"
#include "sched/sched.h"
#include "sched/thread.h"

#define KBD_DATA_PORT  0x60U
#define KBD_STATUS_PORT 0x64U
#define KBD_VECTOR      0x21U  /* IRQ1 = GSI1 → vector 0x21 */

/* Minimal US QWERTY scancode → ASCII map (set 1, make codes). */
static const char s_scan_ascii[128] = {
    0,   0,  '1','2','3','4','5','6','7','8','9','0','-','=', 8,  '\t', /* 0x00-0x0F */
  'q','w','e','r','t','y','u','i','o','p','[',']','\n',  0, 'a', 's',  /* 0x10-0x1F */
  'd','f','g','h','j','k','l',';','\'','`',  0, '\\','z','x','c','v', /* 0x20-0x2F */
  'b','n','m',',','.','/',  0, '*',  0, ' ',  0,  0,  0,  0,  0,  0,  /* 0x30-0x3F */
    0,  0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',   /* 0x40-0x4F */
  '2','3','0','.',  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   /* 0x50-0x5F */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   /* 0x60-0x6F */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   /* 0x70-0x7F */
};

static const char s_scan_ascii_shift[128] = {
    0,   0, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
  'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',  0, 'A', 'S',
  'D','F','G','H','J','K','L',':','"', '~',  0, '|', 'Z','X','C','V',
  'B','N','M','<','>','?',  0, '*',  0, ' ',  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
  '2','3','0','.',  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

#define KBD_BUF_SIZE 64
static volatile char    s_buf[KBD_BUF_SIZE];
static volatile uint8_t s_head;
static volatile uint8_t s_tail;
static volatile bool    s_shift;
static volatile bool    s_caps;
static struct thread   *s_stdin_waiter;

void ps2kbd_set_stdin_waiter(struct thread *t) { s_stdin_waiter = t; }

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Declared in IDT. */
extern void ps2kbd_isr(void);

/* C handler called from assembly stub. */
void ps2kbd_handler(void)
{
    uint8_t scan = inb(KBD_DATA_PORT);
    bool release = (scan & 0x80U) != 0;
    uint8_t code = scan & 0x7fU;

    if (code == 0x2a || code == 0x36) { /* left/right shift */
        s_shift = !release;
        goto eoi;
    }
    if (code == 0x3a && !release) { /* caps lock */
        s_caps = !s_caps;
        goto eoi;
    }
    if (release) goto eoi;

    char c = s_shift ? s_scan_ascii_shift[code] : s_scan_ascii[code];
    if (c >= 'a' && c <= 'z' && s_caps) c = (char)(c - 32);
    if (c == 0) goto eoi;

    uint8_t next = (uint8_t)((s_tail + 1) % KBD_BUF_SIZE);
    if (next != s_head) {
        s_buf[s_tail] = c;
        s_tail = next;
    }
    /* Wake any thread blocked in sys_read(fd=0). */
    if (s_stdin_waiter) {
        struct thread *w = s_stdin_waiter;
        s_stdin_waiter = NULL;
        sched_wake(w);
    }
eoi:
    lapic_eoi();
}

void ps2kbd_init(void)
{
    /* Flush any pending bytes. */
    while (inb(KBD_STATUS_PORT) & 1) inb(KBD_DATA_PORT);

    /* Register ISR — ps2kbd_isr is defined in an ASM stub below.
       We register as a simple C handler via a small trampoline. */
    idt_install_gate(KBD_VECTOR, ps2kbd_isr, 0);

    /* Route IRQ1 (GSI1) to this CPU at vector 0x21. */
    ioapic_route(1, KBD_VECTOR, 0, false, false);
    ioapic_unmask(1);

    serial_write("ps2kbd: IRQ1 enabled\n");
}

char ps2kbd_getchar(void)
{
    if (s_head == s_tail) return 0;
    char c = s_buf[s_head];
    s_head = (uint8_t)((s_head + 1) % KBD_BUF_SIZE);
    return c;
}

bool ps2kbd_ready(void)
{
    return s_head != s_tail;
}
