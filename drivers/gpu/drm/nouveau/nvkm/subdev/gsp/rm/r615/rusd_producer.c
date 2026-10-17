/* SPDX-License-Identifier: MIT */
#include <linux/errno.h>
#include <linux/string.h>

#include "rusd_producer.h"

static void
r615_rusd_producer_release(struct r615_rusd_producer *producer)
{
	producer->ops->release(producer->context);
	memset(producer, 0, sizeof(*producer));
}

int
r615_rusd_producer_stop(struct r615_rusd_producer *producer)
{
	struct r615_rusd_poll_params poll = { 0 };
	struct r615_rusd_init_params init = { 0 };
	int poll_ret, ret;

	if (!producer)
		return -EINVAL;
	if (producer->state == R615_RUSD_STOPPED)
		return 0;
	if (producer->state == R615_RUSD_ALLOCATED) {
		r615_rusd_producer_release(producer);
		return 0;
	}
	producer->state = R615_RUSD_QUARANTINED;
	poll_ret = producer->ops->control(producer->context, R615_RUSD_POLL_CONTROL,
					  &poll, sizeof(poll));
	ret = producer->ops->control(producer->context, R615_RUSD_INIT_CONTROL,
				     &init, sizeof(init));
	if (ret)
		return ret;

	/* An acknowledged unmap, not merely a polling stop, permits DMA release. */
	r615_rusd_producer_release(producer);
	return poll_ret;
}

int
r615_rusd_producer_start(struct r615_rusd_producer *producer,
			 const struct r615_rusd_producer_ops *ops, void *context,
			 u64 poll_mask, u32 interval_ms)
{
	struct r615_rusd_init_params init = { .init = 1 };
	struct r615_rusd_poll_params poll = { .mask = poll_mask, .interval_ms = interval_ms };
	int ret;

	if (!producer || !ops || !ops->allocate || !ops->release || !ops->control ||
	    !poll_mask || (poll_mask & ~((u64)R615_RUSD_POLL_SUPPORTED)) ||
	    interval_ms < R615_RUSD_POLL_MIN_MS)
		return -EINVAL;
	if (producer->state != R615_RUSD_STOPPED)
		return -EBUSY;

	producer->ops = ops;
	producer->context = context;
	ret = ops->allocate(context, R615_RUSD_BUFFER_SIZE, &producer->address,
			    &producer->source);
	if (ret) {
		memset(producer, 0, sizeof(*producer));
		return ret;
	}
	producer->state = R615_RUSD_ALLOCATED;
	if (!producer->address || (producer->address & (R615_RUSD_BUFFER_SIZE - 1)) ||
	    producer->address > ~0ULL - (R615_RUSD_BUFFER_SIZE - 1) ||
	    producer->source.bytes < R615_RUSD_BUFFER_SIZE || !producer->source.copy ||
	    !producer->source.read_barrier) {
		r615_rusd_producer_release(producer);
		return -EINVAL;
	}
	init.address = producer->address;
	/* A failed/timeout reply can still mean that the device saw the address. */
	producer->state = R615_RUSD_REGISTERED;
	ret = ops->control(context, R615_RUSD_INIT_CONTROL, &init, sizeof(init));
	if (!ret)
		ret = ops->control(context, R615_RUSD_POLL_CONTROL, &poll, sizeof(poll));
	if (ret) {
		r615_rusd_producer_stop(producer);
		return ret;
	}
	producer->poll_mask = poll_mask;
	producer->interval_ms = interval_ms;
	producer->state = R615_RUSD_ACTIVE;
	return 0;
}

void
r615_rusd_producer_quiesced(struct r615_rusd_producer *producer)
{
	if (producer && producer->state != R615_RUSD_STOPPED)
		r615_rusd_producer_release(producer);
}

static u64
r615_rusd_metric_mask(enum r615_rusd_metric metric)
{
	switch (metric) {
	case R615_RUSD_GPU_BUSY:
	case R615_RUSD_MEMORY_BUSY:
	case R615_RUSD_ENCODER_BUSY:
	case R615_RUSD_DECODER_BUSY:
		return R615_RUSD_POLL_PERF;
	case R615_RUSD_GPU_CLOCK_HZ:
	case R615_RUSD_MEMORY_CLOCK_HZ:
		return R615_RUSD_POLL_CLOCK;
	case R615_RUSD_GPU_TEMP_MC:
	case R615_RUSD_MEMORY_TEMP_MC:
	case R615_RUSD_BOARD_TEMP_MC:
		return R615_RUSD_POLL_THERMAL;
	case R615_RUSD_GPU_POWER_AVG_UW:
	case R615_RUSD_GPU_POWER_INST_UW:
	case R615_RUSD_GPU_POWER_LIMIT_REQUESTED_UW:
	case R615_RUSD_GPU_POWER_LIMIT_ENFORCED_UW:
		return R615_RUSD_POLL_POWER;
	case R615_RUSD_PMA_TOTAL_BYTES:
	case R615_RUSD_PMA_FREE_BYTES:
		return R615_RUSD_POLL_MEMORY;
	case R615_RUSD_FAN0_RPM:
		return R615_RUSD_POLL_FAN;
	default:
		return 0;
	}
}

int
r615_rusd_producer_read(struct r615_rusd_producer *producer, enum r615_rusd_metric metric,
			u64 now_ns, u64 max_age_ns, struct r615_rusd_value *value)
{
	u64 mask = r615_rusd_metric_mask(metric);

	if (!producer || !mask || !value)
		return -EINVAL;
	if (producer->state != R615_RUSD_ACTIVE || !(producer->poll_mask & mask))
		return -ENODATA;
	return r615_rusd_read(&producer->source, metric, now_ns, max_age_ns, value);
}
