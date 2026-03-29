// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/module.h>
#include <asm/kvm_pkvm.h>
#include <asm/nmi.h>
#include <kunit/test.h>
#include "lapic.h"
#ifdef CONFIG_PKVM_INTEL
#include "vmx/vmx.h"
#endif

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

static void pkvm_check_hyp_mmu(struct kunit *test)
{
	KUNIT_ASSERT_EQ_MSG(test, pkvm_hypercall(test, CHECK_HYP_MMU, pkvm_mem_base, pkvm_mem_size),
			    0, "pkvm-check-hyp-mmu: failed\n");
}

static struct kunit_case pkvm_hyp_mmu_test_cases[] = {
	KUNIT_CASE(pkvm_check_hyp_mmu),
	{}
};

static struct kunit_suite pkvm_hyp_mmu = {
	.name = "pkvm_hyp_mmu",
	.test_cases = pkvm_hyp_mmu_test_cases,
};

#ifdef CONFIG_PKVM_INTEL
static int test_vmxon(u64 vmxon_pointer)
{
	cr4_set_bits(X86_CR4_VMXE);

	asm goto("1: vmxon %[vmxon_pointer]\n\t"
			  _ASM_EXTABLE(1b, %l[fault])
			  : : [vmxon_pointer] "m"(vmxon_pointer)
			  : : fault);
	return -EIO;

fault:
	cr4_clear_bits(X86_CR4_VMXE);
	return 0;
}

static int test_vmxoff(void)
{
	asm goto("1: vmxoff\n\t"
			  _ASM_EXTABLE(1b, %l[fault])
			  ::: "cc", "memory" : fault);

	cr4_clear_bits(X86_CR4_VMXE);
	return -EIO;

fault:
	cr4_clear_bits(X86_CR4_VMXE);
	return 0;
}

static struct vmcs *__alloc_vmcs(gfp_t flags)
{
	struct vmcs *vmcs;
	u64 basic;
	u32 size;

	rdmsrq(MSR_IA32_VMX_BASIC, basic);
	size = vmx_basic_vmcs_size(basic);

	vmcs = (struct vmcs *)__get_free_pages(flags, get_order(size));
	if (!vmcs)
		return NULL;

	memset(vmcs, 0, size);

	/* KVM supports Enlightened VMCS v1 only */
	if (kvm_is_using_evmcs())
		vmcs->hdr.revision_id = KVM_EVMCS_VERSION;
	else
		vmcs->hdr.revision_id = vmx_basic_vmcs_revision_id(basic);

	return vmcs;
}

static void __free_vmcs(struct vmcs *vmcs)
{
	free_page((unsigned long)vmcs);
}

#define vmx_test_asm0(insn)						\
({									\
	int ret = -EIO;							\
	asm goto("1: " __stringify(insn) "\n\t"				\
			  _ASM_EXTABLE(1b, %l[fault])			\
			  : : : "cc" : fault);				\
	goto done;							\
fault:									\
	ret = 0;							\
done:									\
	ret;								\
})

#define vmx_test_asm1(insn, op1)					\
({									\
	int ret = -EIO;							\
	asm goto("1: " __stringify(insn) " %0\n\t"			\
			  _ASM_EXTABLE(1b, %l[fault])			\
			  : : op1 : "cc" : fault);			\
	goto done;							\
fault:									\
	ret = 0;							\
done:									\
	ret;								\
})

#define vmx_test_asm2(insn, op1, op2)					\
({									\
	int ret = -EIO;							\
	asm goto("1: "  __stringify(insn) " %1, %0\n\t"			\
			  _ASM_EXTABLE(1b, %l[fault])			\
			  : : op1, op2 : "cc" : fault);			\
	goto done;							\
fault:									\
	ret = 0;							\
done:									\
	ret;								\
})

static int test_vmwrite(void)
{
	return vmx_test_asm2(vmwrite, "r"((u64)HOST_RSP), "r"((unsigned long)0));
}

static int test_vmread(void)
{
	unsigned long value;

	asm goto("1: vmread %1, %0\n\t"
		 _ASM_EXTABLE(1b, %l[fault])
		 : "=a"(value) : "r"((u64)HOST_RIP): "cc" : fault);

	return -EIO;
fault:
	return 0;
}

static int test_vmclear(struct vmcs *vmcs)
{
	u64 phys_addr = __pa(vmcs);

	return vmx_test_asm1(vmclear, "m"(phys_addr));
}

static int test_vmptrld(struct vmcs *vmcs)
{
	u64 phys_addr = __pa(vmcs);

	return vmx_test_asm1(vmptrld, "m"(phys_addr));
}

static int test_vmptrst(void)
{
	u64 phys_addr = INVALID_PAGE;

	asm goto("1: vmptrst (%0)\n\t"
		     _ASM_EXTABLE(1b, %l[fault])
		     :
		     : "r" (&phys_addr)
		     : "cc", "memory"
		     : fault);
	return -EIO;
fault:
	return 0;
}

static int test_vmlaunch(void)
{
	return vmx_test_asm0(vmlaunch);
}

static int test_vmresume(void)
{
	return vmx_test_asm0(vmresume);
}

static int test_vmfunc(void)
{
	return vmx_test_asm0(vmfunc);
}

static int test_invvpid(void)
{
	struct {
		u64 vpid : 16;
		u64 rsvd : 48;
		u64 gva;
	} operand = { 0, 0, 0 };

	return vmx_test_asm2(invvpid, "r"((u64)VMX_VPID_EXTENT_ALL_CONTEXT), "m"(operand));
}

static int test_invept(void)
{
	struct {
		u64 eptp;
		u64 reserved_0;
	} operand = { 0, 0 };

	return vmx_test_asm2(invept, "r"((u64)VMX_EPT_EXTENT_GLOBAL), "m"(operand));
}

#endif

static void pkvm_vmx_onoff_test(struct kunit *test)
{
#ifndef CONFIG_PKVM_INTEL
	kunit_skip(test, "pkvm-intel is not enabled for test\n");
#else
	int cpu = get_cpu();
	struct vmcs *vmcs;

	if (!(cpuid_ecx(1) & feature_bit(VMX)) ||
	    !this_cpu_has(X86_FEATURE_MSR_IA32_FEAT_CTL) ||
	    !this_cpu_has(X86_FEATURE_VMX)) {
		kunit_skip(test, "pkvm-vmx: VMX not supported on CPU %d\n", cpu);
		return;
	}

	vmcs = __alloc_vmcs(GFP_ATOMIC);
	KUNIT_ASSERT_NOT_NULL_MSG(test, vmcs, "pkvm_vmx: failed to allocate vmcs for CPU%d\n", cpu);

	KUNIT_EXPECT_EQ_MSG(test, test_vmxon(__pa(vmcs)), 0, "pkvm_vmx: expect vmxon failed\n");
	/*
	 * vmxon is failed, so suppose there is no reason to allow vmxoff, as
	 * well as other vmx instructions. But as the pKVM hypervisor emulates
	 * these instructions so still need to execute these instructions to
	 * verify the emulation works as expected.
	 */
	KUNIT_EXPECT_EQ_MSG(test, test_vmxoff(), 0, "pkvm_vmx: expect vmxoff failed\n");
	KUNIT_EXPECT_EQ_MSG(test, test_vmwrite(), 0, "pkvm_vmx: expect vmcs_write failed\n");
	KUNIT_EXPECT_EQ_MSG(test, test_vmread(), 0, "pkvm_vmx: expect vmcs_read failed\n");
	KUNIT_EXPECT_EQ_MSG(test, test_vmclear(vmcs), 0, "pkvm_vmx: expect vmcs_clear failed\n");
	KUNIT_EXPECT_EQ_MSG(test, test_vmptrld(vmcs), 0, "pkvm_vmx: expect vmcs_load failed\n");
	KUNIT_EXPECT_EQ_MSG(test, test_vmptrst(), 0, "pkvm_vmx: expect vmcs_store failed\n");
	KUNIT_EXPECT_EQ_MSG(test, test_vmlaunch(), 0, "pkvm_vmx: expect vmcs_launch failed\n");
	KUNIT_EXPECT_EQ_MSG(test, test_vmresume(), 0, "pkvm_vmx: expect vmcs_resume failed\n");
	KUNIT_EXPECT_EQ_MSG(test, test_vmfunc(), 0, "pkvm_vmx: expect vmcs_function failed\n");
	KUNIT_EXPECT_EQ_MSG(test, test_invvpid(), 0, "pkvm_vmx: expect invvpid failed\n");
	KUNIT_EXPECT_EQ_MSG(test, test_invept(), 0, "pkvm_vmx: expect invept failed\n");

	__free_vmcs(vmcs);

	put_cpu();
#endif
}

static struct kunit_case pkvm_vmx_test_cases[] = {
	KUNIT_CASE(pkvm_vmx_onoff_test),
	{}
};

static struct kunit_suite pkvm_vmx = {
	.name = "pkvm_vmx",
	.test_cases = pkvm_vmx_test_cases,
};

static void pkvm_spec_test(struct kunit *test)
{
	u64 host_spec, val;
	int ret;

	if (!boot_cpu_has(X86_FEATURE_MSR_SPEC_CTRL)) {
		kunit_skip(test, "pkvm-spec: no spec control support\n");
		return;
	}

	ret = pkvm_hypercall(test, SPEC_CTRL);
	if (ret) {
		kunit_skip(test, "pkvm-spec: spec control is not used by pKVM\n");
		return;
	}

	rdmsrq(MSR_IA32_SPEC_CTRL, host_spec);
	wrmsrq(MSR_IA32_SPEC_CTRL, 0);

	KUNIT_EXPECT_EQ_MSG(test, pkvm_hypercall(test, SPEC_CTRL), 0,
			"pkvm-spec: spec control test failed\n");

	rdmsrq(MSR_IA32_SPEC_CTRL, val);

	KUNIT_EXPECT_EQ_MSG(test, val, 0, "pkvm-spec: expect host spec_ctrl to be 0\n");

	wrmsrq(MSR_IA32_SPEC_CTRL, host_spec);
}

static struct kunit_case pkvm_spec_test_cases[] = {
	KUNIT_CASE(pkvm_spec_test),
	{}
};

static struct kunit_suite pkvm_spec = {
	.name = "pkvm_spec",
	.test_cases = pkvm_spec_test_cases,
};

static void pkvm_mem_test(struct kunit *test)
{
	struct pkvm_mem_info infos[] = {
		{
			.pa	= pkvm_mem_base,
			.size	= pkvm_mem_size,
		},
#ifndef CONFIG_PKVM_X86_DEBUG
		{
			.pa	= __pa_symbol(pkvm_sym(text_start)),
			.size	= pkvm_sym(text_end) - pkvm_sym(text_start),
		},
		{
			.pa	= __pa_symbol(pkvm_sym(rodata_start)),
		},
		{
			.pa	= __pa_symbol(pkvm_sym(data_start)),
			.size	= pkvm_sym(data_end) - pkvm_sym(data_start),
		},
		{
			.pa	= __pa_symbol(pkvm_sym(bss_start)),
			.size	= pkvm_sym(bss_end) - pkvm_sym(bss_start),
		},
#endif
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(infos); i++) {
		void *end = __va(PAGE_ALIGN(infos[i].pa + infos[i].size));
		void *start = __va(PAGE_ALIGN_DOWN(infos[i].pa));

		while (start < end) {
			u64 value;

			asm goto("1: movq (%1), %0\n\t"
				 _ASM_EXTABLE(1b, %l[do_exception])
				 : "=r" (value)
				 : "r" (start)
				 : "memory"
				 : do_exception);

			KUNIT_FAIL(test, "pkvm_mem: test failed\n");
			return;
do_exception:
			start += PAGE_SIZE;
		}
	}
}

static struct kunit_case pkvm_mem_test_cases[] = {
	KUNIT_CASE(pkvm_mem_test),
	{}
};

static struct kunit_suite pkvm_mem = {
	.name = "pkvm_mem",
	.test_cases = pkvm_mem_test_cases,
};

static void pkvm_test_perf_ctrl(struct kunit *test)
{
	u64 msr_val;
	u64 msr_val_to_restore;

	rdmsrq(MSR_CORE_PERF_GLOBAL_CTRL, msr_val);
	msr_val_to_restore = msr_val;
	if (!msr_val) {
		// Enable bit 0 (PMC0) and bits 32-34 (Fixed Counters)
		msr_val |= 0x0000000700000001ULL;
		wrmsrq(MSR_CORE_PERF_GLOBAL_CTRL, msr_val);
	}

	KUNIT_ASSERT_EQ_MSG(test, pkvm_hypercall(test, VM_EXIT_ENTRY_CTRLS,
			MSR_CORE_PERF_GLOBAL_CTRL, msr_val),
			    0, "pkvm_test_perf_ctrl: failed\n");

	wrmsrq(MSR_CORE_PERF_GLOBAL_CTRL, msr_val_to_restore);
}

static struct kunit_case pkvm_vm_exit_entry_ctrls_test_cases[] = {
	KUNIT_CASE(pkvm_test_perf_ctrl),
	{}
};

static struct kunit_suite pkvm_vm_exit_entry_ctrls = {
	.name = "pkvm_vm_exit_entry_ctrls",
	.test_cases = pkvm_vm_exit_entry_ctrls_test_cases,
};

kunit_test_suites(&pkvm_nmi, &pkvm_msr, &pkvm_lapic, &pkvm_init_finalize,
		  &pkvm_reprivilege, &pkvm_fix_exception, &pkvm_hyp_mmu,
		  &pkvm_vmx, &pkvm_spec, &pkvm_mem, &pkvm_vm_exit_entry_ctrls);

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
