// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include "pkvm_test.h"
#include "mmu.h"

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
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}
