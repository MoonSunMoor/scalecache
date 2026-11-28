// SPDX-License-Identifier: GPL-2.0-only
/*
 * df_balance_dirty_pages() for pxt4 (ScaleCache dflush).
 *
 * A copy of balance_dirty_pages() and the static helpers it calls, from
 * mm/page-writeback.c of the kernel.  The only change is that a throttled
 * task runs wb_do_writeback_modified() itself after each pause, so that
 * dirtying tasks write back in parallel with the flusher.  pxt4 installs it
 * as balance_dirty_pages_module while the module is loaded.
 *
 * Keep the copy in sync with the kernel pxt4 is built for.  It was taken
 * from Linux 7.2.8.
 */

#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/init.h>
#include <linux/backing-dev.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/blkdev.h>
#include <linux/mpage.h>
#include <linux/rmap.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/sysctl.h>
#include <linux/cpu.h>
#include <linux/syscalls.h>
#include <linux/folio_batch.h>
#include <linux/timer.h>
#include <linux/sched/rt.h>
#include <linux/sched/signal.h>
#include <linux/mm_inline.h>
#include <linux/shmem_fs.h>
#include <trace/events/writeback.h>
#include <linux/memcontrol.h>

#include "dflush.h"

#define BANDWIDTH_INTERVAL	max(HZ/5, 1)

#define RATELIMIT_CALC_SHIFT	10

#ifdef CONFIG_CGROUP_WRITEBACK

#define GDTC_INIT(__wb)		.wb = (__wb),				\
				.dom = &global_wb_domain,		\
				.wb_completions = &(__wb)->completions

#define GDTC_INIT_NO_WB		.dom = &global_wb_domain

#define MDTC_INIT(__wb, __gdtc)	.wb = (__wb),				\
				.dom = mem_cgroup_wb_domain(__wb),	\
				.wb_completions = &(__wb)->memcg_completions, \
				.gdtc = __gdtc

static bool mdtc_valid(struct dirty_throttle_control *dtc)
{
	return dtc->dom;
}

static struct dirty_throttle_control *mdtc_gdtc(struct dirty_throttle_control *mdtc)
{
	return mdtc->gdtc;
}

#else	/* CONFIG_CGROUP_WRITEBACK */

#define GDTC_INIT(__wb)		.wb = (__wb),                           \
				.wb_completions = &(__wb)->completions
#define GDTC_INIT_NO_WB
#define MDTC_INIT(__wb, __gdtc)

static bool mdtc_valid(struct dirty_throttle_control *dtc)
{
	return false;
}

static struct dirty_throttle_control *mdtc_gdtc(struct dirty_throttle_control *mdtc)
{
	return NULL;
}

#endif	/* CONFIG_CGROUP_WRITEBACK */

static inline bool dtc_is_global(struct dirty_throttle_control *dtc)
{
	return mdtc_gdtc(dtc) == NULL;
}

/*
 * Dirty background will ignore pages being written as we're trying to
 * decide whether to put more under writeback.
 */
static void domain_dirty_avail(struct dirty_throttle_control *dtc,
			       bool include_writeback)
{
	if (dtc_is_global(dtc)) {
		dtc->avail = global_dirtyable_memory();
		dtc->dirty = global_node_page_state(NR_FILE_DIRTY);
		if (include_writeback)
			dtc->dirty += global_node_page_state(NR_WRITEBACK);
	} else {
		unsigned long filepages = 0, headroom = 0, writeback = 0;

		mem_cgroup_wb_stats(dtc->wb, &filepages, &headroom, &dtc->dirty,
				    &writeback);
		if (include_writeback)
			dtc->dirty += writeback;
		mdtc_calc_avail(dtc, filepages, headroom);
	}
}

static unsigned long domain_poll_intv(struct dirty_throttle_control *dtc,
				      bool strictlimit)
{
	unsigned long dirty, thresh;

	if (strictlimit) {
		dirty = dtc->wb_dirty;
		thresh = dtc->wb_thresh;
	} else {
		dirty = dtc->dirty;
		thresh = dtc->thresh;
	}

	return dirty_poll_interval(dirty, thresh);
}

/*
 * Throttle it only when the background writeback cannot catch-up. This avoids
 * (excessively) small writeouts when the wb limits are ramping up in case of
 * !strictlimit.
 *
 * In strictlimit case make decision based on the wb counters and limits. Small
 * writeouts when the wb limits are ramping up are the price we consciously pay
 * for strictlimit-ing.
 */
static void domain_dirty_freerun(struct dirty_throttle_control *dtc,
				 bool strictlimit)
{
	unsigned long dirty, thresh, bg_thresh;

	if (unlikely(strictlimit)) {
		wb_dirty_limits(dtc);
		dirty = dtc->wb_dirty;
		thresh = dtc->wb_thresh;
		bg_thresh = dtc->wb_bg_thresh;
	} else {
		dirty = dtc->dirty;
		thresh = dtc->thresh;
		bg_thresh = dtc->bg_thresh;
	}
	dtc->freerun = dirty <= dirty_freerun_ceiling(thresh, bg_thresh);
}

static void balance_domain_limits(struct dirty_throttle_control *dtc,
				  bool strictlimit)
{
	domain_dirty_avail(dtc, true);
	domain_dirty_limits(dtc);
	domain_dirty_freerun(dtc, strictlimit);
}

static void wb_dirty_freerun(struct dirty_throttle_control *dtc,
			     bool strictlimit)
{
	dtc->freerun = false;

	/* was already handled in domain_dirty_freerun */
	if (strictlimit)
		return;

	wb_dirty_limits(dtc);
	/*
	 * LOCAL_THROTTLE tasks must not be throttled when below the per-wb
	 * freerun ceiling.
	 */
	if (!(current->flags & PF_LOCAL_THROTTLE))
		return;

	dtc->freerun = dtc->wb_dirty <
		       dirty_freerun_ceiling(dtc->wb_thresh, dtc->wb_bg_thresh);
}

static inline void wb_dirty_exceeded(struct dirty_throttle_control *dtc,
				     bool strictlimit)
{
	dtc->dirty_exceeded = (dtc->wb_dirty > dtc->wb_thresh) &&
		((dtc->dirty > dtc->thresh) || strictlimit);
}

/*
 * The limits fields dirty_exceeded and pos_ratio won't be updated if wb is
 * in freerun state. Please don't use these invalid fields in freerun case.
 */
static void balance_wb_limits(struct dirty_throttle_control *dtc,
			      bool strictlimit)
{
	wb_dirty_freerun(dtc, strictlimit);
	if (dtc->freerun)
		return;

	wb_dirty_exceeded(dtc, strictlimit);
	wb_position_ratio(dtc);
}

/*
 * balance_dirty_pages() must be called by processes which are generating dirty
 * data.  It looks at the number of dirty pages in the machine and will force
 * the caller to wait once crossing the (background_thresh + dirty_thresh) / 2.
 * If we're over `background_thresh' then the writeback threads are woken to
 * perform some writeout.
 */
int df_balance_dirty_pages(struct bdi_writeback *wb,
			   unsigned long pages_dirtied, unsigned int flags)
{
	struct dirty_throttle_control gdtc_stor = { GDTC_INIT(wb) };
	struct dirty_throttle_control mdtc_stor = { MDTC_INIT(wb, &gdtc_stor) };
	struct dirty_throttle_control * const gdtc = &gdtc_stor;
	struct dirty_throttle_control * const mdtc = mdtc_valid(&mdtc_stor) ?
						     &mdtc_stor : NULL;
	struct dirty_throttle_control *sdtc;
	unsigned long nr_dirty;
	long period;
	long pause;
	long max_pause;
	long min_pause;
	int nr_dirtied_pause;
	unsigned long task_ratelimit;
	unsigned long dirty_ratelimit;
	struct backing_dev_info *bdi = wb->bdi;
	bool strictlimit = bdi->capabilities & BDI_CAP_STRICTLIMIT;
	unsigned long start_time = jiffies;
	int ret = 0;

	for (;;) {
		unsigned long now = jiffies;

		nr_dirty = global_node_page_state(NR_FILE_DIRTY);

		balance_domain_limits(gdtc, strictlimit);
		if (mdtc) {
			/*
			 * If @wb belongs to !root memcg, repeat the same
			 * basic calculations for the memcg domain.
			 */
			balance_domain_limits(mdtc, strictlimit);
		}

		if (!writeback_in_progress(wb) &&
		    (nr_dirty > gdtc->bg_thresh ||
		     (strictlimit && gdtc->wb_dirty > gdtc->wb_bg_thresh)))
			wb_start_background_writeback(wb);

		/*
		 * If memcg domain is in effect, @dirty should be under
		 * both global and memcg freerun ceilings.
		 */
		if (gdtc->freerun && (!mdtc || mdtc->freerun)) {
			unsigned long intv;
			unsigned long m_intv;

free_running:
			intv = domain_poll_intv(gdtc, strictlimit);
			m_intv = ULONG_MAX;

			current->dirty_paused_when = now;
			current->nr_dirtied = 0;
			if (mdtc)
				m_intv = domain_poll_intv(mdtc, strictlimit);
			current->nr_dirtied_pause = min(intv, m_intv);
			break;
		}

		/*
		 * Unconditionally start background writeback if it's not
		 * already in progress. We need to do this because the global
		 * dirty threshold check above (nr_dirty > gdtc->bg_thresh)
		 * doesn't account for the memcg-based throttling case. memcg
		 * uses its own dirty count and thresholds and can trigger
		 * throttling even when global nr_dirty < gdtc->bg_thresh
		 *
		 * Writeback needs to be started else the writer stalls in the
		 * throttle loop waiting for dirty pages to be written back
		 * while no writeback is running.
		 */
		if (unlikely(!writeback_in_progress(wb)))
			wb_start_background_writeback(wb);

		mem_cgroup_flush_foreign(wb);

		/*
		 * Calculate global domain's pos_ratio and select the
		 * global dtc by default.
		 */
		balance_wb_limits(gdtc, strictlimit);
		if (gdtc->freerun)
			goto free_running;
		sdtc = gdtc;

		if (mdtc) {
			/*
			 * If memcg domain is in effect, calculate its
			 * pos_ratio.  @wb should satisfy constraints from
			 * both global and memcg domains.  Choose the one
			 * w/ lower pos_ratio.
			 */
			balance_wb_limits(mdtc, strictlimit);
			if (mdtc->freerun)
				goto free_running;
			if (mdtc->pos_ratio < gdtc->pos_ratio)
				sdtc = mdtc;
		}

		wb->dirty_exceeded = gdtc->dirty_exceeded ||
				     (mdtc && mdtc->dirty_exceeded);
		if (time_is_before_jiffies(READ_ONCE(wb->bw_time_stamp) +
					   BANDWIDTH_INTERVAL))
			__wb_update_bandwidth(gdtc, mdtc, true);

		/* throttle according to the chosen dtc */
		dirty_ratelimit = READ_ONCE(wb->dirty_ratelimit);
		task_ratelimit = ((u64)dirty_ratelimit * sdtc->pos_ratio) >>
							RATELIMIT_CALC_SHIFT;
		max_pause = wb_max_pause(wb, sdtc->wb_dirty);
		min_pause = wb_min_pause(wb, max_pause,
					 task_ratelimit, dirty_ratelimit,
					 &nr_dirtied_pause);

		if (unlikely(task_ratelimit == 0)) {
			period = max_pause;
			pause = max_pause;
			goto pause;
		}
		period = HZ * pages_dirtied / task_ratelimit;
		pause = period;
		if (current->dirty_paused_when)
			pause -= now - current->dirty_paused_when;
		/*
		 * For less than 1s think time (ext3/4 may block the dirtier
		 * for up to 800ms from time to time on 1-HDD; so does xfs,
		 * however at much less frequency), try to compensate it in
		 * future periods by updating the virtual time; otherwise just
		 * do a reset, as it may be a light dirtier.
		 */
		if (pause < min_pause) {
			trace_balance_dirty_pages(wb,
						  sdtc,
						  dirty_ratelimit,
						  task_ratelimit,
						  pages_dirtied,
						  period,
						  min(pause, 0L),
						  start_time);
			if (pause < -HZ) {
				current->dirty_paused_when = now;
				current->nr_dirtied = 0;
			} else if (period) {
				current->dirty_paused_when += period;
				current->nr_dirtied = 0;
			} else if (current->nr_dirtied_pause <= pages_dirtied)
				current->nr_dirtied_pause += pages_dirtied;
			break;
		}
		if (unlikely(pause > max_pause)) {
			/* for occasional dropped task_ratelimit */
			now += min(pause - max_pause, max_pause);
			pause = max_pause;
		}

pause:
		trace_balance_dirty_pages(wb,
					  sdtc,
					  dirty_ratelimit,
					  task_ratelimit,
					  pages_dirtied,
					  period,
					  pause,
					  start_time);
		if (flags & BDP_ASYNC) {
			ret = -EAGAIN;
			break;
		}
		__set_current_state(TASK_KILLABLE);
		bdi->last_bdp_sleep = jiffies;
		io_schedule_timeout(pause);

		/* ScaleCache dflush: the throttled task writes back too. */
		wb_do_writeback_modified(wb);

		current->dirty_paused_when = now + pause;
		current->nr_dirtied = 0;
		current->nr_dirtied_pause = nr_dirtied_pause;

		/*
		 * This is typically equal to (dirty < thresh) and can also
		 * keep "1000+ dd on a slow USB stick" under control.
		 */
		if (task_ratelimit)
			break;

		/*
		 * In the case of an unresponsive NFS server and the NFS dirty
		 * pages exceeds dirty_thresh, give the other good wb's a pipe
		 * to go through, so that tasks on them still remain responsive.
		 *
		 * In theory 1 page is enough to keep the consumer-producer
		 * pipe going: the flusher cleans 1 page => the task dirties 1
		 * more page. However wb_dirty has accounting errors.  So use
		 * the larger and more IO friendly wb_stat_error.
		 */
		if (sdtc->wb_dirty <= wb_stat_error())
			break;

		if (fatal_signal_pending(current))
			break;
	}
	return ret;
}
