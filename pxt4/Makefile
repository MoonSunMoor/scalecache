# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the pxt4 filesystem (ext4 on the ScaleCache page cache) and
# its journal jbd3, built out of tree against a ScaleCache kernel:
#   make KDIR=<kernel build directory>
#

ifneq ($(KERNELRELEASE),)

obj-m += pxt4.o jbd3/

pxt4-y	:= balloc.o bitmap.o block_validity.o dir.o pxt4_jbd3.o extents.o \
		extents_status.o file.o fsmap.o fsync.o hash.o ialloc.o \
		indirect.o inline.o inode.o ioctl.o mballoc.o migrate.o \
		mmp.o move_extent.o namei.o page-io.o readpage.o resize.o \
		super.o symlink.o sysfs.o xattr.o xattr_hurd.o xattr_trusted.o \
		xattr_user.o fast_commit.o orphan.o

# ScaleCache dflush: df_balance_dirty_pages()
pxt4-y	+= mm/page-writeback.o

# pxt4 has no Kconfig of its own and follows the kernel's ext4 options.
pxt4-$(CONFIG_EXT4_FS_POSIX_ACL)	+= acl.o
pxt4-$(CONFIG_EXT4_FS_SECURITY)		+= xattr_security.o
pxt4-$(CONFIG_FS_VERITY)		+= verity.o
pxt4-$(CONFIG_FS_ENCRYPTION)		+= crypto.o
ccflags-$(CONFIG_EXT4_FS_POSIX_ACL)	+= -DCONFIG_PXT4_FS_POSIX_ACL
ccflags-$(CONFIG_EXT4_FS_SECURITY)	+= -DCONFIG_PXT4_FS_SECURITY
ccflags-$(CONFIG_EXT4_DEBUG)		+= -DCONFIG_PXT4_DEBUG

else

KDIR	?= /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
