#include <linux/kernel.h>
#include <linux/syscalls.h>

#include "tifa-utils.h"

SYSCALL_DEFINE1(changeReadPolicy, int, newval)
{
	readPolicy = newval;
	switch (readPolicy) {
	case 0:
		printk("Normal read\n");
		break;
	case 1:
		printk("EBUSY (Random under >=2GCs)\n");
		break;
	case 2:
		printk("GCT (BRT under >=2GCs)\n");
		break;
	case 3:
		printk("IOD3 (TW-only)\n");
		break;
	default:
		printk("Unknown Policy\n");
		break;
	}
	return 0;
}
