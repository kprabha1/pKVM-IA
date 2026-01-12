// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>

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
