#ifndef KERNEL_CORE_PANIC_H
#define KERNEL_CORE_PANIC_H

void panic(const char *message) __attribute__((noreturn));

#endif

