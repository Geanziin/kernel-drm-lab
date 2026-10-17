/* SPDX-License-Identifier: MIT */
#ifndef __NOUVEAU_ENGINE_BUSY_H__
#define __NOUVEAU_ENGINE_BUSY_H__
#include <linux/spinlock.h>
#include <linux/types.h>

struct dma_fence;
struct nouveau_drm;

/* Driver-side video engine busy accounting.
 *
 * The GSP-RM video perfmon (0x20802087) and RUSD engUtil report 0 for work
 * submitted by nouveau, so the busy fraction is measured from the jobs this
 * driver submits: an engine counts as busy while at least one job on a
 * channel bound to it has been handed to the hardware and its fence has not
 * signalled.  This is "time with outstanding work", not a PMU clock counter.
 */
enum nouveau_busy_engine {
	NOUVEAU_BUSY_NONE = 0,
	NOUVEAU_BUSY_NVENC,
	NOUVEAU_BUSY_NVDEC,
	NOUVEAU_BUSY_NR,
};

struct nouveau_engine_busy {
	spinlock_t lock;
	unsigned int active;	/* jobs in flight */
	u64 busy_since;		/* ns, valid while active */
	u64 busy_ns;		/* accumulated busy time */
	u64 sample_time, sample_busy; /* last reader sample */
	unsigned int sample_pct;
};

void nouveau_engine_busy_init(struct nouveau_engine_busy *);
/* Account one job; returns 0 or -errno (accounting skipped, job unaffected). */
int nouveau_engine_busy_track(struct nouveau_engine_busy *, struct dma_fence *);
/* Busy percent since the previous read (min window 200 ms). */
unsigned int nouveau_engine_busy_percent(struct nouveau_engine_busy *);
/* Pure helper used by the reader (exported for CPU tests). */
unsigned int nouveau_engine_busy_pct(u64 busy_delta, u64 wall_delta);
#endif
