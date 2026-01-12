/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_PKVM_TEST_H
#define __PKVM_X86_PKVM_TEST_H

#include <asm/kvm_pkvm.h>

#ifdef CONFIG_PKVM_X86_HYP_TEST
int pkvm_test(struct kvm_vcpu *hvcpu, union pkvm_hc_data *in,
	      union pkvm_hc_data *out);
#else
static inline int pkvm_test(struct kvm_vcpu *hvcpu, union pkvm_hc_data *in,
			    union pkvm_hc_data *out)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __PKVM_X86_PKVM_TEST_H */
