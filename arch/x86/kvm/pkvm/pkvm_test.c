// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include "pkvm_test.h"

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
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}
