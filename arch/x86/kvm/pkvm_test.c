// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/module.h>
#include <asm/kvm_pkvm.h>
#include <asm/nmi.h>
#include <kunit/test.h>

/* check to see if NMI IPIs work on this machine */
static DECLARE_BITMAP(nmi_ipi_mask, NR_CPUS);

static bool busy_loop;
#define PKVM_DELAY_MS	100

static void pkvm_busy_loop(void *unused)
{
	smp_store_release(&busy_loop, true);
	pkvm_hypercall(test, NMI_BUSY_LOOP, PKVM_DELAY_MS);
	smp_store_release(&busy_loop, false);
}

static int test_nmi_ipi_callback(unsigned int val, struct pt_regs *regs)
{
        int cpu = raw_smp_processor_id();

        if (cpumask_test_and_clear_cpu(cpu, to_cpumask(nmi_ipi_mask)))
                return NMI_HANDLED;

        return NMI_DONE;
}

static void test_nmi(struct kunit *test, struct cpumask *mask)
{
	unsigned long timeout;

	KUNIT_ASSERT_EQ_MSG(test, register_nmi_handler(NMI_LOCAL, test_nmi_ipi_callback,
						       NMI_FLAG_FIRST, "pkvm_nmi_test"),
			    0, "pkvm-nmi: failed to register nmi handler\n");

	/* sync above data before sending NMI */
	wmb();

	/*
	 * At least delay 1ms to make sure the pKVM is in busy loop
	 * on the remote CPU.
	 */
	timeout = PKVM_DELAY_MS;
	do {
		mdelay(1);
	} while (!smp_load_acquire(&busy_loop) && --timeout);

	KUNIT_EXPECT_NE_MSG(test, timeout, 0,
		"pkvm-nmi: expect remote CPU in busy loop but not before sending NMI\n");

	__apic_send_IPI_mask(mask, NMI_VECTOR);

	KUNIT_EXPECT_NE_MSG(test, smp_load_acquire(&busy_loop), 0,
		"pkvm-nmi: expect remote CPU is in busy loop but not after sending NMI.\n");

	/* Don't wait longer than two times of pKVM delay time */
	timeout = PKVM_DELAY_MS * 2;
	while (!cpumask_empty(mask) && --timeout)
	        mdelay(1);

	KUNIT_EXPECT_NE_MSG(test, timeout, 0,
		"pkvm-nmi: timeout to receive NMI\n");

	/* What happens if we timeout, do we still unregister?? */
	unregister_nmi_handler(NMI_LOCAL, "pkvm_nmi_test");
}

static void pkvm_nmi_test(struct kunit *test)
{
	int self = raw_smp_processor_id();
	int remote;

	remote = (self + 1) % num_online_cpus();
	cpumask_set_cpu(remote, to_cpumask(nmi_ipi_mask));

	KUNIT_ASSERT_EQ(test, smp_call_function_single(remote, pkvm_busy_loop, NULL, 0), 0);

	test_nmi(test, to_cpumask(nmi_ipi_mask));
}

static struct kunit_case pkvm_nmi_test_cases[] = {
	KUNIT_CASE(pkvm_nmi_test),
	{}
};

static struct kunit_suite pkvm_nmi = {
	.name = "pkvm_nmi",
	.test_cases = pkvm_nmi_test_cases,
};

kunit_test_suites(&pkvm_nmi);

static int __init pkvm_kunit_test_init(void)
{
	return 0;
}
module_init(pkvm_kunit_test_init);

static void __exit pkvm_kunit_test_exit(void)
{
}
module_exit(pkvm_kunit_test_exit);

MODULE_DESCRIPTION("pKVM x86 kunit test cases");
MODULE_LICENSE("GPL");
