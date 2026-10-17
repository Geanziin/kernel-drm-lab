// SPDX-License-Identifier: MIT
/* Driver-side NVENC/NVDEC busy accounting; see nouveau_engine_busy.h. */
#include <linux/dma-fence.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/slab.h>

#include "nouveau_engine_busy.h"

#define NOUVEAU_BUSY_MIN_WINDOW_NS (200ULL * NSEC_PER_MSEC)

struct nouveau_engine_busy_cb {
	struct dma_fence_cb cb;
	struct nouveau_engine_busy *eb;
};

void
nouveau_engine_busy_init(struct nouveau_engine_busy *eb)
{
	spin_lock_init(&eb->lock);
	eb->active = 0;
	eb->busy_ns = 0;
	eb->sample_time = ktime_get_ns();
	eb->sample_busy = 0;
	eb->sample_pct = 0;
}

unsigned int
nouveau_engine_busy_pct(u64 busy_delta, u64 wall_delta)
{
	if (!wall_delta)
		return 0;
	if (busy_delta >= wall_delta)
		return 100;
	return (unsigned int)div64_u64(busy_delta * 100 + wall_delta / 2, wall_delta);
}

static void
nouveau_engine_busy_end(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct nouveau_engine_busy_cb *ecb = container_of(cb, typeof(*ecb), cb);
	struct nouveau_engine_busy *eb = ecb->eb;
	unsigned long flags;
	u64 now = ktime_get_ns();

	spin_lock_irqsave(&eb->lock, flags);
	if (!WARN_ON_ONCE(!eb->active) && --eb->active == 0)
		eb->busy_ns += now - eb->busy_since;
	spin_unlock_irqrestore(&eb->lock, flags);
	kfree(ecb);
}

int
nouveau_engine_busy_track(struct nouveau_engine_busy *eb, struct dma_fence *fence)
{
	struct nouveau_engine_busy_cb *ecb;
	unsigned long flags;
	int ret;

	/* run_job context: never block on reclaim for an accounting record. */
	ecb = kmalloc(sizeof(*ecb), GFP_NOWAIT);
	if (!ecb)
		return -ENOMEM;
	ecb->eb = eb;

	spin_lock_irqsave(&eb->lock, flags);
	if (eb->active++ == 0)
		eb->busy_since = ktime_get_ns();
	spin_unlock_irqrestore(&eb->lock, flags);

	ret = dma_fence_add_callback(fence, &ecb->cb, nouveau_engine_busy_end);
	if (ret) {
		/* Already signalled: the job finished, close the interval now. */
		nouveau_engine_busy_end(fence, &ecb->cb);
		return 0;
	}
	return 0;
}

unsigned int
nouveau_engine_busy_percent(struct nouveau_engine_busy *eb)
{
	unsigned long flags;
	u64 now = ktime_get_ns(), busy;

	spin_lock_irqsave(&eb->lock, flags);
	busy = eb->busy_ns + (eb->active ? now - eb->busy_since : 0);
	if (now - eb->sample_time >= NOUVEAU_BUSY_MIN_WINDOW_NS) {
		eb->sample_pct = nouveau_engine_busy_pct(busy - eb->sample_busy,
							 now - eb->sample_time);
		eb->sample_time = now;
		eb->sample_busy = busy;
	}
	busy = eb->sample_pct;
	spin_unlock_irqrestore(&eb->lock, flags);
	return (unsigned int)busy;
}
