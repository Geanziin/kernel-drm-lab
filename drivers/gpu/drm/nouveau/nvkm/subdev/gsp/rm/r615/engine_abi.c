// SPDX-License-Identifier: MIT
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "engine_abi.h"

static_assert(sizeof(struct r615_channel_alloc) == 376);
static_assert(sizeof(struct r615_display_static) == 32);
static_assert(sizeof(struct r615_display_active) == 20);
static_assert(sizeof(struct r615_dp_caps) == 64);
static_assert(sizeof(struct r615_fifo_device_entry) == 104);
static_assert(sizeof(struct r615_fifo_device_info) == 3340);
static_assert(offsetof(struct r615_fifo_device_info, entries) == 12);

int
r615_channel_build(const struct r615_channel_config *cfg, struct r615_channel_alloc *out)
{
	if (!cfg || !out || cfg->chid >= 4096 || cfg->runqueue > 1 ||
	    !cfg->gpfifo_bytes || cfg->gpfifo_bytes % 8 || cfg->gpfifo % 8 ||
	    cfg->instance % 4096 || cfg->instance_bytes < 512 ||
	    !cfg->userd_bytes || !cfg->methods_bytes ||
	    cfg->instance > ~0ULL - cfg->instance_bytes ||
	    cfg->userd > ~0ULL - cfg->userd_bytes || cfg->methods > ~0ULL - cfg->methods_bytes)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	out->gpfifo = cfg->gpfifo;
	out->entries = cfg->gpfifo_bytes / 8;
	out->flags = cfg->runqueue << 4 | (u32)cfg->privileged << 5 |
		     (cfg->chid % 8) << 8 | (cfg->chid / 8) << 12 | 1U << 21;
	out->vaspace = cfg->vaspace;
	out->engine = cfg->engine;
	out->instance = (struct r615_memory_desc) { cfg->instance, cfg->instance_bytes, 2, 1 };
	out->userd_mem = (struct r615_memory_desc) { cfg->userd, cfg->userd_bytes, 2, 1 };
	out->ramfc = (struct r615_memory_desc) { cfg->instance, 512, 2, 1 };
	out->methods = (struct r615_memory_desc) { cfg->methods, cfg->methods_bytes, 1, 0 };
	/* Public alloc_channel.h: USER/ADMIN privilege and NONE notifiers. */
	out->internal_flags = (cfg->privileged ? 1U : 0U) | 1U << 2 | 1U << 4;
	return 0;
}
