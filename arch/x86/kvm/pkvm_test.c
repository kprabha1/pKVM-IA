// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/module.h>
#include <asm/kvm_pkvm.h>
#include <asm/nmi.h>
#include <kunit/test.h>
#include "lapic.h"

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

static void pkvm_supported_msr_test(struct kunit *test)
{
	union cpuid10_eax eax = {
		.full = native_cpuid_eax(10),
	};
	u64 val;

	if (eax.split.version_id > 1) {
		KUNIT_ASSERT_EQ_MSG(test, rdmsrq_safe(MSR_CORE_PERF_GLOBAL_CTRL, &val), 0,
				"pkvm-msr: read MSR_CORE_PERF_GLOBAL_CTRL failed\n");
		KUNIT_ASSERT_EQ_MSG(test, wrmsrq_safe(MSR_CORE_PERF_GLOBAL_CTRL, val), 0,
				"pkvm-msr: write MSR_CORE_PERF_GLOBAL_CTRL failed\n");
	} else {
		KUNIT_ASSERT_NE_MSG(test, rdmsrq_safe(MSR_CORE_PERF_GLOBAL_CTRL, &val), 0,
				"pkvm-msr: expect MSR_CORE_PERF_GLOBAL_CTRL unsupported\n");
	}

	KUNIT_ASSERT_EQ_MSG(test, rdmsrq_safe(MSR_IA32_APICBASE, &val), 0,
		"pkvm-msr: read MSR_IA32_APICBASE failed\n");
	KUNIT_ASSERT_EQ_MSG(test, wrmsrq_safe(MSR_IA32_APICBASE, val), 0,
		"pkvm-msr: write MSR_IA32_APICBASE failed\n");
}

static void pkvm_unconditional_trapped_msr_test(struct kunit *test)
{
	u32 msr = 0xC0002000;
	u64 val;

	KUNIT_ASSERT_NE_MSG(test, rdmsrq_safe(msr, &val), 0,
		"pkvm-msr: Expect failure to read unconditional trapped MSR 0x%x\n", msr);

	KUNIT_ASSERT_NE_MSG(test, wrmsrq_safe(msr, val), 0,
		"pkvm-msr: Expect failure to write unconditional trapped MSR 0x%x\n", msr);
}

static struct kunit_case pkvm_msr_test_cases[] = {
	KUNIT_CASE(pkvm_supported_msr_test),
	KUNIT_CASE(pkvm_unconditional_trapped_msr_test),
	{}
};

static struct kunit_suite pkvm_msr = {
	.name = "pkvm_msr",
	.test_cases = pkvm_msr_test_cases,
};

static void pkvm_lapic_base_test(struct kunit *test)
{
	u64 apic_base;

	KUNIT_ASSERT_EQ_MSG(test, rdmsrq_safe(MSR_IA32_APICBASE, &apic_base),
			    0, "pkvm-lapic: failed to read apic base\n");

	KUNIT_ASSERT_EQ_MSG(test, (apic_base & LAPIC_MODE_X2APIC), LAPIC_MODE_X2APIC,
			    "pkvm-lapic: invalid apic base 0x%llx\n", apic_base);

	KUNIT_ASSERT_NE_MSG(test, wrmsrq_safe(MSR_IA32_APICBASE, apic_base & (~LAPIC_MODE_X2APIC)),
			    0, "pkvm-lapic: expect failure to disable lapic but not\n");
}

static void pkvm_lapic_id_test(struct kunit *test)
{
	u64 apic_id;

	KUNIT_ASSERT_EQ_MSG(test, rdmsrq_safe(X2APIC_MSR(APIC_ID), &apic_id),
			    0, "pkvm-lapic: failed to read apic id\n");

	KUNIT_ASSERT_NE_MSG(test, wrmsrq_safe(X2APIC_MSR(APIC_ID), apic_id + 1),
			    0, "pkvm-lapic: expect failure to change apic id but not\n");

	KUNIT_ASSERT_EQ_MSG(test, wrmsrq_safe(X2APIC_MSR(APIC_ID), apic_id),
			    0, "pkvm-lapic: failed to write apic id with the same value\n");
}

static struct kunit_case pkvm_lapic_test_cases[] = {
	KUNIT_CASE(pkvm_lapic_base_test),
	KUNIT_CASE(pkvm_lapic_id_test),
	{}
};

static struct kunit_suite pkvm_lapic = {
	.name = "pkvm_lapic",
	.test_cases = pkvm_lapic_test_cases,
};

static void do_pkvm_hyp_init(void *data)
{
	struct pkvm_mem_info infos[] = {
		{
			.type	= PKVM_TEXT_DATA,
			.va	= (unsigned long)pkvm_sym(text_start),
			.pa	= __pa_symbol(pkvm_sym(text_start)),
			.size	= pkvm_sym(text_end) - pkvm_sym(text_start),
			.prot	= pgprot_val(PAGE_KERNEL_EXEC),
		},
	};
	int ret = pkvm_hypercall(init, (unsigned long)infos, ARRAY_SIZE(infos));

	if (data)
		*(int *)data = ret;
}

static void pkvm_init_finalize_test(struct kunit *test)
{
	int init_ret, cpu;

	for_each_possible_cpu(cpu) {
		KUNIT_ASSERT_EQ_MSG(test, smp_call_function_single(cpu, do_pkvm_hyp_init,
								   &init_ret, 1), 0,
				    "pkvm-init-finalize: CPU%d smp-call failed\n", cpu);
		KUNIT_ASSERT_NE_MSG(test, init_ret, 0,
				    "pkvm-init-finalize: expect init failure on CPU %d\n", cpu);
	}

	KUNIT_ASSERT_NE_MSG(test, pkvm_hypercall(init_finalize), 0,
			    "pkvm-init-finalize: expect init_finalize failure\n");
}

static struct kunit_case pkvm_init_finalize_test_cases[] = {
	KUNIT_CASE(pkvm_init_finalize_test),
	{}
};

static struct kunit_suite pkvm_init_finalize = {
	.name = "pkvm_init_finalize",
	.test_cases = pkvm_init_finalize_test_cases,
};

static void reprivilege_cpu(void *data)
{
	int ret = pkvm_hypercall(reprivilege_cpu);

	if (data)
		*(int *)data = ret;
}

static void pkvm_reprivilege_test(struct kunit *test)
{
	int cpu, repriv_ret;

	for_each_possible_cpu(cpu) {
		KUNIT_ASSERT_EQ_MSG(test, smp_call_function_single(cpu, reprivilege_cpu,
								   &repriv_ret, 1), 0,
			"pkvm-reprivilege: CPU%d smp-call failed\n", cpu);
		KUNIT_ASSERT_NE_MSG(test, repriv_ret, 0,
			"pkvm-reprivilege: expect reprivilege failure on CPU %d\n", cpu);
	}
}

static struct kunit_case pkvm_reprivilege_test_cases[] = {
	KUNIT_CASE(pkvm_reprivilege_test),
	{}
};

static struct kunit_suite pkvm_reprivilege = {
	.name = "pkvm_reprivilege",
	.test_cases = pkvm_reprivilege_test_cases,
};

static void pkvm_fix_exception_test(struct kunit *test)
{
	KUNIT_ASSERT_EQ_MSG(test, pkvm_hypercall(test, FIX_EXCEPTION), 0,
		"pkvm-fix-exception: failed\n");
}

static struct kunit_case pkvm_fix_exception_test_cases[] = {
	KUNIT_CASE(pkvm_fix_exception_test),
	{}
};

static struct kunit_suite pkvm_fix_exception = {
	.name = "pkvm_fix_exception",
	.test_cases = pkvm_fix_exception_test_cases,
};

kunit_test_suites(&pkvm_nmi, &pkvm_msr, &pkvm_lapic, &pkvm_init_finalize,
		  &pkvm_reprivilege, &pkvm_fix_exception);

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
