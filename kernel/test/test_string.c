#include "test/ktest.h"

#include <stddef.h>
#include <stdint.h>

#include "lib/string.h"

void test_string_run(void)
{
	ktest_suite("string");

	/* strlen */
	KTEST_ASSERT(strlen("") == 0U);
	KTEST_ASSERT(strlen("hello") == 5U);
	KTEST_ASSERT(strlen("ab\0cd") == 2U);

	/* memset */
	uint8_t buf[8];
	memset(buf, 0xab, sizeof(buf));
	KTEST_ASSERT(buf[0] == 0xabU);
	KTEST_ASSERT(buf[7] == 0xabU);
	memset(buf, 0, sizeof(buf));
	KTEST_ASSERT(buf[3] == 0U);

	/* memcpy */
	uint8_t src[4] = {1, 2, 3, 4};
	uint8_t dst[4] = {0, 0, 0, 0};
	memcpy(dst, src, 4);
	KTEST_ASSERT(dst[0] == 1U);
	KTEST_ASSERT(dst[3] == 4U);

	/* memcmp */
	KTEST_ASSERT(memcmp("abc", "abc", 3) == 0);
	KTEST_ASSERT(memcmp("abc", "abd", 3) < 0);
	KTEST_ASSERT(memcmp("abd", "abc", 3) > 0);
	KTEST_ASSERT(memcmp("abc", "abc", 0) == 0);

	/* memmove: non-overlapping */
	uint8_t mb[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	memmove(mb + 4, mb, 4);
	KTEST_ASSERT(mb[4] == 1U);
	KTEST_ASSERT(mb[7] == 4U);
	KTEST_ASSERT(mb[0] == 1U);  /* source intact */

	/* memmove: overlapping, dest > src */
	uint8_t mb2[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	memmove(mb2 + 2, mb2, 6);
	KTEST_ASSERT(mb2[2] == 1U);
	KTEST_ASSERT(mb2[7] == 6U);
}
