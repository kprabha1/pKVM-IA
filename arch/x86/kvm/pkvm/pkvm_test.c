// SPDX-License-Identifier: GPL-2.0
#include "pkvm_test.h"

int pkvm_test(struct kvm_vcpu *hvcpu, union pkvm_hc_data *in,
	      union pkvm_hc_data *out)
{
	return -EOPNOTSUPP;
}
