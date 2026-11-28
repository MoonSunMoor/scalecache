/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _PXT4_DFLUSH_H
#define _PXT4_DFLUSH_H

struct bdi_writeback;
struct dirty_throttle_control;

/* In the ScaleCache kernel (mm/page-writeback.c): replaces balance_dirty_pages(). */
extern int (*balance_dirty_pages_module)(struct bdi_writeback *wb,
		unsigned long pages_dirtied, unsigned int flags);

/* Exported by the dflush kernel change, but not declared in <linux/writeback.h>. */
void wb_dirty_limits(struct dirty_throttle_control *dtc);

int df_balance_dirty_pages(struct bdi_writeback *wb,
			   unsigned long pages_dirtied, unsigned int flags);

#endif /* _PXT4_DFLUSH_H */
