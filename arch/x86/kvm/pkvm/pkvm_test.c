// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include "pkvm_test.h"
#include "mmu.h"
#include <vmx/vmx.h>
#include <vmx/vmx_ops.h>


unsigned long loops_per_jiffy;
DEFINE_PER_CPU_PAGE_ALIGNED(struct tss_struct, cpu_tss_rw);
DEFINE_PER_CPU_READ_MOSTLY(struct cpuinfo_x86, cpu_info);

static int pkvm_busy_loop(struct kvm_vcpu *hvcpu, unsigned long ms)
{
	mdelay(ms);
	return 0;
}

static int pkvm_fix_exception(void)
{
	asm goto("1:" ASM_UD2 "\n\t"
		     _ASM_EXTABLE(1b, %l[do_exception])
		     ARCH_WARN_REACHABLE
		     : : : : do_exception);

	return -EIO;

do_exception:
	return 0;
}

static int pkvm_check_hyp_mmu(unsigned long pkvm_mem_base, unsigned long pkvm_mem_size)
{
	struct pkvm_mem_info infos[] = {
		{
			.va	= (unsigned long)__va(pkvm_mem_base),
			.size	= pkvm_mem_size,
			.prot	= pgprot_val(PAGE_KERNEL),
		},
#ifndef CONFIG_PKVM_X86_DEBUG
		{
			.va	= (unsigned long)pkvm_sym(rodata_start),
			.size	= pkvm_sym(rodata_end) - pkvm_sym(rodata_start),
			.prot	= pgprot_val(PAGE_KERNEL_RO),
		},
		{
			.va	= (unsigned long)pkvm_sym(data_start),
			.size	= pkvm_sym(data_end) - pkvm_sym(data_start),
			.prot	= pgprot_val(PAGE_KERNEL),
		},
		{
			.va	= (unsigned long)pkvm_sym(bss_start),
			.size	= pkvm_sym(bss_end) - pkvm_sym(bss_start),
			.prot	= pgprot_val(PAGE_KERNEL),
		},
#endif
	};
	unsigned long phys;
	int i, level;
	u64 prot;

	for (i = 0; i < ARRAY_SIZE(infos); i++) {
		unsigned long start = infos[i].va;
		unsigned long end = start + infos[i].size;

		while (start < end) {
			pkvm_hyp_mmu_lookup(start, &phys, &prot, &level);
			if (prot != infos[i].prot)
				return -EINVAL;
			start += PAGE_SIZE;
		}
	}

	return 0;
}

static int pkvm_test_spec_ctrl(void)
{
	u64 spec;

	if (!cpu_feature_enabled(X86_FEATURE_MSR_SPEC_CTRL))
		return 0;

	rdmsrq(MSR_IA32_SPEC_CTRL, spec);

	return (spec == 0) ? -EINVAL : 0;
}

static int validate_perf_global_ctrl(u32 vmexit_ctrls, u32 vmentry_ctrls)
{
	u64 msr_val;

	if ((vmexit_ctrls & VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL)) {
		msr_val = vmcs_read64(HOST_IA32_PERF_GLOBAL_CTRL);
		if (msr_val != 0) {
			pr_err("%s: HOST_IA32_PERF_GLOBAL_CTRL is expected to be 0 in VMCS host state, but got 0x%llx\n",
				__func__, msr_val);
			return -EINVAL;
		}
	} else {
		pr_err("%s: VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL should be set in VM-Exit controls\n", __func__);
		return -EINVAL;
	}

	if (!(vmentry_ctrls & VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL)) {
		pr_err("%s: VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL should be set in VM-Entry controls\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static int validate_perf_rtit_ctrl(u32 vmexit_ctrls, u32 vmentry_ctrls)
{
	int ret = 0;

	if (!(vmexit_ctrls & VM_EXIT_CLEAR_IA32_RTIT_CTL)) {
		pr_err("%s: VM_EXIT_CLEAR_IA32_RTIT_CTL should be set in VM-Exit controls\n", __func__);
		ret = -EINVAL;
	}
	if (!(vmexit_ctrls & VM_EXIT_PT_CONCEAL_PIP)) {
		pr_err("%s: VM_EXIT_PT_CONCEAL_PIP should be set in VM-Exit controls\n", __func__);
		ret |= -EINVAL;
	}

	if (!(vmentry_ctrls & VM_ENTRY_LOAD_IA32_RTIT_CTL)) {
		pr_err("%s: VM_ENTRY_LOAD_IA32_RTIT_CTL should be set in VM-Entry controls\n", __func__);
		ret |= -EINVAL;
	}

	if (!(vmentry_ctrls & VM_ENTRY_PT_CONCEAL_PIP)) {
		pr_err("%s: VM_ENTRY_PT_CONCEAL_PIP should be set in VM-Entry controls\n", __func__);
		ret |= -EINVAL;
	}

	return ret;
}

static int pkvm_test_perf_ctrl(u32 msr_index, u64 msr_val_from_host)
{
	u64 msr_val;
	u32 vmexit_ctrls, vmentry_ctrls;
	int ret;

	rdmsrq(msr_index, msr_val);
	if (msr_val == msr_val_from_host)
		ret = -EINVAL;

	vmentry_ctrls = vmcs_read32(VM_ENTRY_CONTROLS);
	vmexit_ctrls = vmcs_read32(VM_EXIT_CONTROLS);

	switch (msr_index) {
	case MSR_CORE_PERF_GLOBAL_CTRL:
		ret |= validate_perf_global_ctrl(vmexit_ctrls, vmentry_ctrls);
		break;
	case MSR_IA32_RTIT_CTL:
		ret |= validate_perf_rtit_ctrl(vmexit_ctrls, vmentry_ctrls);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int pkvm_test(struct kvm_vcpu *hvcpu, union pkvm_hc_data *in,
	      union pkvm_hc_data *out)
{
	enum pkvm_test_fn test_fn = pkvm_hc_input1(hvcpu);
	int ret = 0;

	switch (test_fn) {
	case NMI_BUSY_LOOP:
		ret = pkvm_busy_loop(hvcpu, pkvm_hc_input2(hvcpu));
		break;
	case FIX_EXCEPTION:
		ret = pkvm_fix_exception();
		break;
	case CHECK_HYP_MMU:
		ret = pkvm_check_hyp_mmu(pkvm_hc_input2(hvcpu), pkvm_hc_input3(hvcpu));
		break;
	case SPEC_CTRL:
		ret = pkvm_test_spec_ctrl();
		break;
	case VM_EXIT_ENTRY_CTRLS:
		ret = pkvm_test_perf_ctrl(pkvm_hc_input2(hvcpu), pkvm_hc_input3(hvcpu));
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}
