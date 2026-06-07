#include "test/ktest.h"

#include "core/serial.h"

static const char *current_suite = "";
static uint64_t passes;
static uint64_t fails;

void ktest_suite(const char *name)
{
	current_suite = name;
	serial_write("ktest: [");
	serial_write(name);
	serial_write("]\n");
}

void ktest_check(const char *expr, int passed)
{
	if (passed) {
		++passes;
	} else {
		++fails;
		serial_write("ktest: FAIL (");
		serial_write(current_suite);
		serial_write(") ");
		serial_write(expr);
		serial_write("\n");
	}
}

uint64_t ktest_fail_count(void)
{
	return fails;
}

void ktest_report(void)
{
	serial_write("ktest: ");
	serial_write_dec(passes + fails);
	serial_write(" run, ");
	serial_write_dec(fails);
	serial_write(" failed\n");
}
