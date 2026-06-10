#include "test/tests.h"

#include "core/panic.h"
#include "test/ktest.h"

void ktest_run_all(void)
{
	test_string_run();
	test_pmm_run();
	test_vmm_run();
	test_heap_run();
	test_sched_run();
	test_userspace_run();
	test_vfs_run();
	test_cred_run();
	ktest_report();

	if (ktest_fail_count() != 0U) {
		panic("kernel self-tests failed");
	}
}
