# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the jbd3 journaling routines (jbd2 for pxt4).  Built from
# ../Makefile together with pxt4.ko, or alone:
#   make KDIR=<kernel build directory>
#

ifneq ($(KERNELRELEASE),)

obj-m += jbd3.o

jbd3-objs := transaction.o commit.o recovery.o checkpoint.o revoke.o journal.o

ccflags-$(CONFIG_JBD2_DEBUG)	+= -DCONFIG_JBD3_DEBUG

else

KDIR	?= /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
