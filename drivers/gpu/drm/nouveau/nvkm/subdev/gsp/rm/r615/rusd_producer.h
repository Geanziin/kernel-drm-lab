/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_RUSD_PRODUCER_H__
#define __NVKM_R615_RUSD_PRODUCER_H__

#include "rusd.h"

#define R615_RUSD_INIT_CONTROL 0x20800afe
#define R615_RUSD_POLL_CONTROL 0x20800aff
#define R615_RUSD_BUFFER_SIZE 4096
#define R615_RUSD_POLL_CLOCK 0x01
#define R615_RUSD_POLL_PERF 0x02
#define R615_RUSD_POLL_MEMORY 0x04
#define R615_RUSD_POLL_POWER 0x08
#define R615_RUSD_POLL_THERMAL 0x10
#define R615_RUSD_POLL_PCI 0x20
#define R615_RUSD_POLL_FAN 0x40
#define R615_RUSD_POLL_SUPPORTED 0x7f
#define R615_RUSD_POLL_MIN_MS 100

struct r615_rusd_init_params {
	u8 init;
	u8 reserved[7];
	u64 address;
};

struct r615_rusd_poll_params {
	u64 mask;
	u32 interval_ms;
	u32 reserved;
};

enum r615_rusd_producer_state {
	R615_RUSD_STOPPED,
	R615_RUSD_ALLOCATED,
	R615_RUSD_REGISTERED,
	R615_RUSD_ACTIVE,
	R615_RUSD_QUARANTINED,
};

/* The adapter serializes callers, verifies the 615 transport and timestamp domain,
 * and supplies zeroed, contiguous coherent DMA memory that remains pinned until
 * release. It must not connect this interface to an unverified firmware ABI.
 */
struct r615_rusd_producer_ops {
	int (*allocate)(void *context, size_t bytes, u64 *address,
			struct r615_rusd_source *source);
	void (*release)(void *context);
	int (*control)(void *context, u32 command, const void *params, size_t bytes);
};

struct r615_rusd_producer {
	const struct r615_rusd_producer_ops *ops;
	void *context;
	struct r615_rusd_source source;
	enum r615_rusd_producer_state state;
	u64 address;
	u64 poll_mask;
	u32 interval_ms;
};

int r615_rusd_producer_start(struct r615_rusd_producer *,
			     const struct r615_rusd_producer_ops *, void *context,
			     u64 poll_mask, u32 interval_ms);
int r615_rusd_producer_stop(struct r615_rusd_producer *);
/* Call only after the GSP's DMA has actually stopped, including failed teardown. */
void r615_rusd_producer_quiesced(struct r615_rusd_producer *);
int r615_rusd_producer_read(struct r615_rusd_producer *, enum r615_rusd_metric,
			    u64 now_ns, u64 max_age_ns, struct r615_rusd_value *);

#endif
