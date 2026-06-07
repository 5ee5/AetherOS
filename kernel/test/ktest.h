#ifndef KERNEL_TEST_KTEST_H
#define KERNEL_TEST_KTEST_H

#include <stdint.h>

void ktest_suite(const char *name);
void ktest_check(const char *expr, int passed);
uint64_t ktest_fail_count(void);
void ktest_report(void);

#define KTEST_ASSERT(expr) ktest_check(#expr, (expr) != 0)

#endif
