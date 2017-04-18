/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/rcupdate.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/swap.h>
#include <linux/fs.h>
#include <linux/cpuset.h>
#include <linux/show_mem_notifier.h>
#include <linux/vmpressure.h>
#include <linux/log_jank.h>
#ifdef CONFIG_HUWEI_LMK_DEBUG
#include <linux/rtc.h>
#include <linux/debugfs.h>
#endif

#define CREATE_TRACE_POINTS
#include <trace/events/almk.h>

#ifdef CONFIG_HIGHMEM
#define _ZONE ZONE_HIGHMEM
#else
#define _ZONE ZONE_NORMAL
#endif
#ifdef CONFIG_HUAWEI_KSTATE
#include <linux/hw_kcollect.h>
#endif

#ifdef CONFIG_HUWEI_LMK_DEBUG
static uint32_t lowmem_debug_level = 2;
#else
static uint32_t lowmem_debug_level = 1;
#endif

#ifdef CONFIG_HUAWEI_KERNEL_DEBUG
extern ssize_t write_log_to_exception(const char* category, char level, const char* msg);
extern int command_to_exception(char* tag, char* msg);
#define CMD_SIZE     256
#define TIME_BUF_SIZE 16
#define LMK_MSG_SIZE 512
char lmk_msg[LMK_MSG_SIZE];
bool report_to_exception = false;
#endif

#ifdef CONFIG_HUWEI_LOW_MEMORY_KILLER

/* the lmk_remit_process max size is LMK_REMIT_SIZE*/
#define LMK_REMIT_SIZE 8
static char lmk_remit_process[][TASK_COMM_LEN] = {
    "lowmeminfo",
    "vold"
};
#endif

static short lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};
static int lowmem_adj_size = 4;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;
static int lmk_fast_run = 1;

#ifdef CONFIG_HISI_MULTI_KILL
/*
 * lmk_multi_kill: select open/close multi kill
 * if lmk_multi_kill open
 *    1/selected process adj >= lmk_multi_fadj,
 *      we kill multi process max count = lmk_multi_fcount,
 *    2/selected process adj < lmk_multi_fadj,
 *      select process adj >= lmk_multi_sadj,
 *      we kill multi process max count = lmk_multi_scount,
 *    3/selected process adj < lmk_multi_sadj,
 *      we kill one process,
 */
static int lmk_multi_kill;
static int lmk_multi_fadj = 470;
static int lmk_multi_fcount = 5;
static int lmk_multi_sadj = 176;
static int lmk_multi_scount = 3;
static int lmk_timeout_inter = 1;
#endif

#ifdef CONFIG_LOG_JANK
static ulong lowmem_kill_count = 0;
static ulong lowmem_free_mem = 0;
#endif
static unsigned long lowmem_deathpending_timeout;

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			pr_info(x);			\
	} while (0)

static atomic_t shift_adj = ATOMIC_INIT(0);
static short adj_max_shift = 353;

/* User knob to enable/disable adaptive lmk feature */
static int enable_adaptive_lmk;
module_param_named(enable_adaptive_lmk, enable_adaptive_lmk, int,
	S_IRUGO | S_IWUSR);

/*
 * This parameter controls the behaviour of LMK when vmpressure is in
 * the range of 90-94. Adaptive lmk triggers based on number of file
 * pages wrt vmpressure_file_min, when vmpressure is in the range of
 * 90-94. Usually this is a pseudo minfree value, higher than the
 * highest configured value in minfree array.
 */
static int vmpressure_file_min;
module_param_named(vmpressure_file_min, vmpressure_file_min, int,
	S_IRUGO | S_IWUSR);

enum {
	VMPRESSURE_NO_ADJUST = 0,
	VMPRESSURE_ADJUST_ENCROACH,
	VMPRESSURE_ADJUST_NORMAL,
};

#ifdef CONFIG_HUWEI_LOW_MEMORY_KILLER
/*************************************************
*  Function:    do_remit_process
*  Description: check the process is in the lmk_remit_process
*  Input:
*  Output:
*  Return:      if the process is in the lmk_remit_process , return true, otherwise return false
*  Others:
*************************************************/
static bool do_remit_process(struct task_struct *p)
{
    int i;

    lowmem_print(4,"process name %s\n", p->comm);
    for ( i = 0; i < LMK_REMIT_SIZE; i++) {
		if (!strcmp(p->comm, lmk_remit_process[i])){
            break;
	    }
    }

    if (i < LMK_REMIT_SIZE) {
        return true;
    } else {
        return false;
    }
}



/*************************************************
*  Function:    hw_tune_lmk_param
*  Description: tune the process's adj
*  Input:
*  Output:
*  Return:
*  Others:
*************************************************/
static void hw_tune_lmk_param(short * score_adj, struct task_struct *p)
{
    if (*score_adj < 100)
        return;

    if (do_remit_process(p)) {
        lowmem_print(4,"the process '%s' adj(%d) is remit process, so reduce 100 adj\n",
                p->comm, *score_adj);
        *score_adj -= 100;
    }

    return;
}
#endif

#ifdef CONFIG_HUAWEI_KERNEL_DEBUG
/*************************************************
*  Function:    get_time_stamp
*  Description: get the system time
*  Input:
*  Output:
*  Return:
*  Others:
*************************************************/
static void get_time_stamp(char* timestamp_buf,  unsigned int len)
{
   struct timeval tv;
   struct rtc_time tm;
   if(NULL == timestamp_buf)
   {
       pr_err("timestamp is NULL\n");
       return;
   }
   memset(&tv, 0, sizeof(struct timeval));
   memset(&tm, 0, sizeof(struct rtc_time));
   do_gettimeofday(&tv);
   tv.tv_sec -= sys_tz.tz_minuteswest * 60; /* times: 60s*/
   rtc_time_to_tm(tv.tv_sec, &tm);
   snprintf(timestamp_buf,len,"%04d%02d%02d%02d%02d%02d",tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/*************************************************
*  Function:    report_lmkill_to_exception
*  Description: report the lowmemory kill log to apr
*  Input:
*  Output:
*  Return:
*  Others:
*************************************************/
void report_lmkill_to_exception(void)
{
    char cmd[CMD_SIZE] = {0};
    char time_buf[TIME_BUF_SIZE] = {0};
    int ret = 0;

    get_time_stamp(time_buf,16);
    snprintf(cmd, CMD_SIZE, "archive -i %s -i %s -i %s -o %s_%s -z zip",
       "/data/log/android_logs/kmsgcat-log",
       "/data/log/android_logs/applogcat-log",
       "/sys/kernel/debug/lmk_status",
       time_buf, "lowmem");

    ret = command_to_exception("lowmem", cmd);
    if(ret < 0 )
        lowmem_print(3, "cmd exception sysfs err.\n");
}
#endif

int adjust_minadj(short *min_score_adj)
{
	int ret = VMPRESSURE_NO_ADJUST;

	if (!enable_adaptive_lmk)
		return 0;

	if (atomic_read(&shift_adj) &&
		(*min_score_adj > adj_max_shift)) {
		if (*min_score_adj == OOM_SCORE_ADJ_MAX + 1)
			ret = VMPRESSURE_ADJUST_ENCROACH;
		else
			ret = VMPRESSURE_ADJUST_NORMAL;
		*min_score_adj = adj_max_shift;
	}
	atomic_set(&shift_adj, 0);

	return ret;
}

static int lmk_vmpressure_notifier(struct notifier_block *nb,
			unsigned long action, void *data)
{
	int other_free, other_file;
	unsigned long pressure = action;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (!enable_adaptive_lmk)
		return 0;

	if (pressure >= 95) {
		other_file = global_page_state(NR_FILE_PAGES) -
			global_page_state(NR_SHMEM) -
			total_swapcache_pages();
		other_free = global_page_state(NR_FREE_PAGES);

		atomic_set(&shift_adj, 1);
		trace_almk_vmpressure(pressure, other_free, other_file);
	} else if (pressure >= 90) {
		if (lowmem_adj_size < array_size)
			array_size = lowmem_adj_size;
		if (lowmem_minfree_size < array_size)
			array_size = lowmem_minfree_size;

		other_file = global_page_state(NR_FILE_PAGES) -
			global_page_state(NR_SHMEM) -
			total_swapcache_pages();

		other_free = global_page_state(NR_FREE_PAGES);

		if ((other_free < lowmem_minfree[array_size - 1]) &&
			(other_file < vmpressure_file_min)) {
				atomic_set(&shift_adj, 1);
				trace_almk_vmpressure(pressure, other_free,
					other_file);
		}
	} else if (atomic_read(&shift_adj)) {
		/*
		 * shift_adj would have been set by a previous invocation
		 * of notifier, which is not followed by a lowmem_shrink yet.
		 * Since vmpressure has improved, reset shift_adj to avoid
		 * false adaptive LMK trigger.
		 */
		trace_almk_vmpressure(pressure, other_free, other_file);
		atomic_set(&shift_adj, 0);
	}

	return 0;
}

static struct notifier_block lmk_vmpr_nb = {
	.notifier_call = lmk_vmpressure_notifier,
};

static int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static DEFINE_MUTEX(scan_mutex);

int can_use_cma_pages(gfp_t gfp_mask)
{
	int can_use = 0;
	int mtype = allocflags_to_migratetype(gfp_mask);
	int i = 0;
	int *mtype_fallbacks = get_migratetype_fallbacks(mtype);

	if (is_migrate_cma(mtype)) {
		can_use = 1;
	} else {
		for (i = 0;; i++) {
			int fallbacktype = mtype_fallbacks[i];

			if (is_migrate_cma(fallbacktype)) {
				can_use = 1;
				break;
			}

			if (fallbacktype == MIGRATE_RESERVE)
				break;
		}
	}
	return can_use;
}

void tune_lmk_zone_param(struct zonelist *zonelist, int classzone_idx,
					int *other_free, int *other_file,
					int use_cma_pages)
{
	struct zone *zone;
	struct zoneref *zoneref;
	int zone_idx;

	for_each_zone_zonelist(zone, zoneref, zonelist, MAX_NR_ZONES) {
		zone_idx = zonelist_zone_idx(zoneref);
		if (zone_idx == ZONE_MOVABLE) {
			if (!use_cma_pages && other_free)
				*other_free -=
				    zone_page_state(zone, NR_FREE_CMA_PAGES);
			continue;
		}

		if (zone_idx > classzone_idx) {
			if (other_free != NULL)
				*other_free -= zone_page_state(zone,
							       NR_FREE_PAGES);
			if (other_file != NULL)
				*other_file -= zone_page_state(zone,
							       NR_FILE_PAGES)
					- zone_page_state(zone, NR_SHMEM)
					- zone_page_state(zone, NR_SWAPCACHE);
		} else if (zone_idx < classzone_idx) {
			if (zone_watermark_ok(zone, 0, 0, classzone_idx, 0) &&
			    other_free) {
				if (!use_cma_pages) {
					*other_free -= min(
					  zone->lowmem_reserve[classzone_idx] +
					  zone_page_state(
					    zone, NR_FREE_CMA_PAGES),
					  zone_page_state(
					    zone, NR_FREE_PAGES));
				} else {
					*other_free -=
					  zone->lowmem_reserve[classzone_idx];
				}
			} else {
				if (other_free)
					*other_free -=
					  zone_page_state(zone, NR_FREE_PAGES);
			}
		}
	}
}

#ifdef CONFIG_HIGHMEM
void adjust_gfp_mask(gfp_t *gfp_mask)
{
	struct zone *preferred_zone;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx;

	if (current_is_kswapd()) {
		zonelist = node_zonelist(0, *gfp_mask);
		high_zoneidx = gfp_zone(*gfp_mask);
		first_zones_zonelist(zonelist, high_zoneidx, NULL,
				&preferred_zone);

		if (high_zoneidx == ZONE_NORMAL) {
			if (zone_watermark_ok_safe(preferred_zone, 0,
					high_wmark_pages(preferred_zone), 0,
					0))
				*gfp_mask |= __GFP_HIGHMEM;
		} else if (high_zoneidx == ZONE_HIGHMEM) {
			*gfp_mask |= __GFP_HIGHMEM;
		}
	}
}
#else
void adjust_gfp_mask(gfp_t *unused)
{
}
#endif

void tune_lmk_param(int *other_free, int *other_file, struct shrink_control *sc)
{
	gfp_t gfp_mask;
	struct zone *preferred_zone;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx, classzone_idx;
	unsigned long balance_gap;
	int use_cma_pages;

	gfp_mask = sc->gfp_mask;
	adjust_gfp_mask(&gfp_mask);

	zonelist = node_zonelist(0, gfp_mask);
	high_zoneidx = gfp_zone(gfp_mask);
	first_zones_zonelist(zonelist, high_zoneidx, NULL, &preferred_zone);
	classzone_idx = zone_idx(preferred_zone);
	use_cma_pages = can_use_cma_pages(gfp_mask);

	balance_gap = min(low_wmark_pages(preferred_zone),
			  (preferred_zone->present_pages +
			   KSWAPD_ZONE_BALANCE_GAP_RATIO-1) /
			   KSWAPD_ZONE_BALANCE_GAP_RATIO);

	if (likely(current_is_kswapd() && zone_watermark_ok(preferred_zone, 0,
			  high_wmark_pages(preferred_zone) + SWAP_CLUSTER_MAX +
			  balance_gap, 0, 0))) {
		if (lmk_fast_run)
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       other_file, use_cma_pages);
		else
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       NULL, use_cma_pages);

		if (zone_watermark_ok(preferred_zone, 0, 0, _ZONE, 0)) {
			if (!use_cma_pages) {
				*other_free -= min(
				  preferred_zone->lowmem_reserve[_ZONE]
				  + zone_page_state(
				    preferred_zone, NR_FREE_CMA_PAGES),
				  zone_page_state(
				    preferred_zone, NR_FREE_PAGES));
			} else {
				*other_free -=
				  preferred_zone->lowmem_reserve[_ZONE];
			}
		} else {
			*other_free -= zone_page_state(preferred_zone,
						      NR_FREE_PAGES);
		}

		lowmem_print(4, "lowmem_shrink of kswapd tunning for highmem "
			     "ofree %d, %d\n", *other_free, *other_file);
	} else {
		tune_lmk_zone_param(zonelist, classzone_idx, other_free,
			       other_file, use_cma_pages);

		if (!use_cma_pages) {
			*other_free -=
			  zone_page_state(preferred_zone, NR_FREE_CMA_PAGES);
		}

		lowmem_print(4, "lowmem_shrink tunning for others ofree %d, "
			     "%d\n", *other_free, *other_file);
	}
}

static int lowmem_shrink(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	int rem = 0;
	int tasksize;
	int i;
	int ret = 0;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0;
	int selected_tasksize = 0;
	short selected_oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free;
	int other_file;
	unsigned long nr_to_scan = sc->nr_to_scan;
#ifdef CONFIG_HUAWEI_KERNEL_DEBUG
	/* judge if killing the process of the adj == 0
	 * 0: not kill the adj 0
	 * 1: kill the adj 0
	 */
	int kill_adj_0 = 0;
#endif
#ifdef CONFIG_HISI_MULTI_KILL
	int count = 0;
#endif

	if (nr_to_scan > 0) {
		if (mutex_lock_interruptible(&scan_mutex) < 0)
			return 0;
	}

	other_free = global_page_state(NR_FREE_PAGES);

	if (global_page_state(NR_SHMEM) + total_swapcache_pages() <
		global_page_state(NR_FILE_PAGES))
		other_file = global_page_state(NR_FILE_PAGES) -
						global_page_state(NR_SHMEM) -
						total_swapcache_pages();
	else
		other_file = 0;

	tune_lmk_param(&other_free, &other_file, sc);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		minfree = lowmem_minfree[i];
		if (other_free < minfree && other_file < minfree) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}
	if (nr_to_scan > 0) {
		ret = adjust_minadj(&min_score_adj);
		lowmem_print(3, "lowmem_shrink %lu, %x, ofree %d %d, ma %hd\n",
				nr_to_scan, sc->gfp_mask, other_free,
				other_file, min_score_adj);
	}

	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
	if (nr_to_scan <= 0 || min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		lowmem_print(5, "lowmem_shrink %lu, %x, return %d\n",
			     nr_to_scan, sc->gfp_mask, rem);

		if (nr_to_scan > 0)
			mutex_unlock(&scan_mutex);

		if ((min_score_adj == OOM_SCORE_ADJ_MAX + 1) &&
			(nr_to_scan > 0))
			trace_almk_shrink(0, ret, other_free, other_file, 0);

		return rem;
	}
	selected_oom_score_adj = min_score_adj;

#ifdef CONFIG_HISI_MULTI_KILL
kill_selected:
#endif
	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		/* if task no longer has any memory ignore it */
		if (test_task_flag(tsk, TIF_MM_RELEASED))
			continue;

		if (time_before_eq(jiffies, lowmem_deathpending_timeout)) {
			if (test_task_flag(tsk, TIF_MEMDIE)) {
				rcu_read_unlock();
				/* give the system time to free up the memory */
				msleep_interruptible(20);
				mutex_unlock(&scan_mutex);
				return 0;
			}
		}

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		oom_score_adj = p->signal->oom_score_adj;
#ifdef CONFIG_HUWEI_LOW_MEMORY_KILLER
        hw_tune_lmk_param(&oom_score_adj, p);
#endif
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_score_adj < selected_oom_score_adj)
				continue;
			if (oom_score_adj == selected_oom_score_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_score_adj = oom_score_adj;
		lowmem_print(3, "select '%s' (%d), adj %hd, size %d, to kill\n",
			     p->comm, p->pid, oom_score_adj, tasksize);
	}


#ifdef CONFIG_HUWEI_LOW_MEMORY_KILLER
    if ( (current) && !(current->flags & PF_KTHREAD)) {
        if (selected_oom_score_adj <= 0) {
			selected = current;
            //selected_tasksize = get_mm_rss(current->mm);
            selected_oom_score_adj = current->signal->oom_score_adj;
            lowmem_print(1,"because the selected process '%s' (%d) oom_score_adj is %d,\n" \
                     " so lmk should to kill the current process '%s' (%d) oom_score_adj is %d.\n",
                     selected->comm, selected->pid, selected_oom_score_adj,
                     current->comm, current->pid, current->signal->oom_score_adj);
        }
    }
#endif

	if (selected) {
		/*LOG_JANK_D(JLID_SYS_LMK,"LMK#%s((%d,%hd,%ld,%s,%d),(%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,0x%x),(%ld,%ld,%hd))",
			     selected->comm, selected->pid, selected_oom_score_adj,
			     selected_tasksize * (long)(PAGE_SIZE / 1024), current->comm, current->pid,
			     other_free * (long)(PAGE_SIZE / 1024),
			     global_page_state(NR_FREE_CMA_PAGES) * (long)(PAGE_SIZE / 1024),
			     totalreserve_pages * (long)(PAGE_SIZE / 1024),
			     global_page_state(NR_FREE_PAGES) * (long)(PAGE_SIZE / 1024),
			     global_page_state(NR_FILE_PAGES) * (long)(PAGE_SIZE / 1024),
			     global_page_state(NR_SLAB_RECLAIMABLE) * (long)(PAGE_SIZE / 1024),
			     global_page_state(NR_SLAB_UNRECLAIMABLE) * (long)(PAGE_SIZE / 1024),
			     global_page_state(NR_SLAB_RECLAIMABLE) * (long)(PAGE_SIZE / 1024) +
			     global_page_state(NR_SLAB_UNRECLAIMABLE) * (long)(PAGE_SIZE / 1024),
			     sc->gfp_mask, other_file * (long)(PAGE_SIZE / 1024), minfree * (long)(PAGE_SIZE / 1024), min_score_adj);*/
		lowmem_print(1, "Killing '%s' (%d), adj %hd,\n" \
				"   to free %ldkB on behalf of '%s' (%d) because\n" \
				"   cache %ldkB is below limit %ldkB for oom_score_adj %hd\n" \
				"   Free memory is %ldkB above reserved.\n" \
				"   Free CMA is %ldkB\n" \
				"   Total reserve is %ldkB\n" \
				"   Total free pages is %ldkB\n" \
				"   Total file cache is %ldkB\n" \
				"   Slab Reclaimable is %ldkB\n" \
				"   Slab UnReclaimable is %ldkB\n" \
				"   Total Slab is %ldkB\n" \
				"   GFP mask is 0x%x\n",
			     selected->comm, selected->pid,
			     selected_oom_score_adj,
			     selected_tasksize * (long)(PAGE_SIZE / 1024),
			     current->comm, current->pid,
			     other_file * (long)(PAGE_SIZE / 1024),
			     minfree * (long)(PAGE_SIZE / 1024),
			     min_score_adj,
			     other_free * (long)(PAGE_SIZE / 1024),
			     global_page_state(NR_FREE_CMA_PAGES) *
				(long)(PAGE_SIZE / 1024),
			     totalreserve_pages * (long)(PAGE_SIZE / 1024),
			     global_page_state(NR_FREE_PAGES) *
				(long)(PAGE_SIZE / 1024),
			     global_page_state(NR_FILE_PAGES) *
				(long)(PAGE_SIZE / 1024),
			     global_page_state(NR_SLAB_RECLAIMABLE) *
				(long)(PAGE_SIZE / 1024),
			     global_page_state(NR_SLAB_UNRECLAIMABLE) *
				(long)(PAGE_SIZE / 1024),
			     global_page_state(NR_SLAB_RECLAIMABLE) *
				(long)(PAGE_SIZE / 1024) +
			     global_page_state(NR_SLAB_UNRECLAIMABLE) *
				(long)(PAGE_SIZE / 1024),
			     sc->gfp_mask);

#ifdef CONFIG_HUWEI_LMK_DEBUG
        if (lowmem_debug_level >= 2 && selected_oom_score_adj < 1000) {
#else
        if (lowmem_debug_level >= 2 && selected_oom_score_adj == 0) {
#endif
			/* move the write function down below */
#ifdef CONFIG_HUAWEI_KERNEL_DEBUG
			kill_adj_0 = 1;
#endif
			show_mem(SHOW_MEM_FILTER_NODES);
			dump_tasks(NULL, NULL);
			show_mem_call_notifiers();
		}

#ifdef CONFIG_HISI_MULTI_KILL
		lowmem_deathpending_timeout = jiffies + lmk_timeout_inter * HZ;
#else
		lowmem_deathpending_timeout = jiffies + HZ;
#endif
#ifdef CONFIG_LOG_JANK

		lowmem_kill_count++;
		lowmem_free_mem += selected_tasksize * (long)(PAGE_SIZE / 1024) / 1024;
#endif
#ifdef CONFIG_HUWEI_LMK_DEBUG
        if(do_remit_process(selected) || (selected_oom_score_adj < 100) ) {
		    memset(lmk_msg, 0, LMK_MSG_SIZE);
            snprintf(lmk_msg, LMK_MSG_SIZE, "the process '%s' pid '%d' adj '%d' is killed by lmk\n",
                selected->comm, selected->pid, selected_oom_score_adj);
			report_to_exception = true;
        }
#endif
#ifdef CONFIG_HUAWEI_KSTATE
        hwkillinfo(selected->tgid, 0);
#endif
		send_sig(SIGKILL, selected, 0);
		set_tsk_thread_flag(selected, TIF_MEMDIE);
		rem -= selected_tasksize;
		rcu_read_unlock();
#ifdef CONFIG_HUAWEI_KERNEL_DEBUG
		if (1 == kill_adj_0)
			write_log_to_exception("LMK-EXCEPTION", 'C', "lower memory killer exception");
#endif
		/* give the system time to free up the memory */
		msleep_interruptible(20);
		trace_almk_shrink(selected_tasksize, ret,
			other_free, other_file, selected_oom_score_adj);

#ifdef CONFIG_HUWEI_LMK_DEBUG
        if (report_to_exception) {
		    report_to_exception = false;
#ifdef CONFIG_ENABLE_LMK_APRLOG
		    report_lmkill_to_exception();
#endif
		}
#endif
	} else {
		trace_almk_shrink(1, ret, other_free, other_file, 0);
		rcu_read_unlock();
	}

#ifdef CONFIG_HISI_MULTI_KILL
	if (selected && lmk_multi_kill) {
		count++;
		if (!((count >= lmk_multi_fcount) ||
			(selected_oom_score_adj < lmk_multi_sadj) ||
			((selected_oom_score_adj < lmk_multi_fadj) &&
				(count >= lmk_multi_scount)))) {
			selected = NULL;
			goto kill_selected;
		}
	}
#endif

	lowmem_print(4, "lowmem_shrink %lu, %x, return %d\n",
		     nr_to_scan, sc->gfp_mask, rem);
	mutex_unlock(&scan_mutex);
	return rem;
}

#ifdef CONFIG_HUWEI_LMK_DEBUG
/*************************************************
*  Function:    lmk_status_show
*  Description: show the low memory kill log
*  Input:
*  Output:
*  Return:
*  Others:
*************************************************/
static int lmk_status_show(struct seq_file *m, void *v)
{
    seq_printf(m,"%s",lmk_msg);
    memset(lmk_msg, 0, LMK_MSG_SIZE);
    return 0;
}

/*************************************************
*  Function:    lmk_status_open
*  Description: open function
*  Input:
*  Output:
*  Return:
*  Others:
*************************************************/
static int lmk_status_open(struct inode *inode, struct file *file)
{
    return single_open(file, lmk_status_show, NULL);
}

static const struct file_operations lmk_status_fops = {
	.owner		= THIS_MODULE,
	.open		= lmk_status_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

static int __init lowmem_init(void)
{
#ifdef CONFIG_HUWEI_LMK_DEBUG
    struct dentry *dentry;
#endif

	register_shrinker(&lowmem_shrinker);
	vmpressure_notifier_register(&lmk_vmpr_nb);

#ifdef CONFIG_HUWEI_LMK_DEBUG
	dentry = debugfs_create_file("lmk_status", S_IRUGO, NULL, NULL,
				     &lmk_status_fops);
	if (!dentry)
		lowmem_print(1,"Failed to create the debugfs lmk_status file\n");
#endif

	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
}

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static short lowmem_oom_adj_to_oom_score_adj(short oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	short oom_adj;
	short oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %d => oom_score_adj %d\n",
			     oom_adj, oom_score_adj);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values();

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_short,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
__module_param_call(MODULE_PARAM_PREFIX, adj,
		    &lowmem_adj_array_ops,
		    .arr = &__param_arr_adj,
		    S_IRUGO | S_IWUSR, -1);
__MODULE_PARM_TYPE(adj, "array of short");
#else
module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(lmk_fast_run, lmk_fast_run, int, S_IRUGO | S_IWUSR);
#ifdef CONFIG_LOG_JANK
module_param_named(kill_count, lowmem_kill_count, ulong, S_IRUGO | S_IWUSR);
module_param_named(free_mem, lowmem_free_mem, ulong, S_IRUGO | S_IWUSR);
#endif
#ifdef CONFIG_HISI_MULTI_KILL
module_param_named(lmk_multi_kill, lmk_multi_kill, int, S_IRUGO | S_IWUSR);
module_param_named(lmk_multi_fadj, lmk_multi_fadj, int, S_IRUGO | S_IWUSR);
module_param_named(lmk_multi_fcount, lmk_multi_fcount, int, S_IRUGO | S_IWUSR);
module_param_named(lmk_multi_sadj, lmk_multi_sadj, int, S_IRUGO | S_IWUSR);
module_param_named(lmk_multi_scount, lmk_multi_scount, int, S_IRUGO | S_IWUSR);
module_param_named(lmk_timeout_inter, lmk_timeout_inter, int,
			S_IRUGO | S_IWUSR);
#endif

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");

