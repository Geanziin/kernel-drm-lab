// SPDX-License-Identifier: MIT
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "health.h"

static_assert(sizeof(struct r615_fault_queue) == 192);
static_assert(offsetof(struct r615_fault_queue, get) == 64);
static_assert(offsetof(struct r615_fault_queue, put) == 128);
static_assert(sizeof(struct r615_shadow_register) == 24032);
static_assert(sizeof(struct r615_fecs_enable) == 24);

int
r615_fault_plan(const struct r615_gmmu_static *info, struct r615_fault_plan *out)
{
	u64 total;

	if (!info || !out || info->fault_bytes < 2 * R615_FAULT_BYTES ||
	    info->fault_bytes % R615_FAULT_BYTES)
		return -EINVAL;
	total = (((u64)info->fault_bytes + 4095) & ~4095ULL) +
		(((u64)info->fault_meta + 4095) & ~4095ULL);
	if (total / 4096 > R615_GMMU_MAX_PAGES)
		return -E2BIG;
	*out = (struct r615_fault_plan) {
		.bytes = info->fault_bytes, .metadata_bytes = info->fault_meta,
		.total_bytes = total, .pages = total / 4096,
		.capacity = info->fault_bytes / R615_FAULT_BYTES,
	};
	return 0;
}

int
r615_fault_decode(const void *packet, struct r615_fault_record *out)
{
	u32 word[8];
	struct r615_fault_record fault = { 0 };

	if (!packet || !out)
		return -EINVAL;
	memcpy(word, packet, sizeof(word));
	if (!(word[7] & 0x80000000U))
		return -EPROTO;
	/* Replayable and confidential-compute packets are different consumers. */
	if (word[7] & ((1U << 7) | (1U << 29)))
		return -EOPNOTSUPP;
	fault.aperture = (word[0] >> 8) & 3;
	if (fault.aperture == 1)
		return -EPROTO;
	fault.instance = (u64)word[1] << 32 | (word[0] & 0xfffff000U);
	fault.address = (u64)word[3] << 32 | (word[2] & 0xfffff000U);
	fault.timestamp = (u64)word[5] << 32 | word[4];
	fault.engine = word[6] & 0x1ff;
	fault.type = word[7] & 0x1f;
	fault.client = (word[7] >> 8) & 0x7f;
	fault.access = (word[7] >> 16) & 0xf;
	fault.hub = (word[7] >> 20) & 1;
	fault.gpc = (word[7] >> 24) & 0x1f;
	*out = fault;
	return 0;
}

int
r615_recovery_update(struct r615_recovery_state *state, u32 type, u8 value, u32 scratch)
{
	struct r615_recovery_state next;

	if (!state || type > 4 || value > 1)
		return -EINVAL;
	if (scratch == ~0U)
		return -ENODEV;
	next = *state;
	if (type) {
		if (value)
			next.requests |= 1U << type;
		else
			next.requests &= ~(1U << type);
	}
	next.reset = !!(scratch & 1);
	next.drain = !!(scratch & 2);
	/* Same precedence as public gpuRefreshRecoveryAction_KERNEL. A request
	 * for reset/reboot is a requirement, not authority to reset the host.
	 */
	if (next.requests & (1U << 3)) next.action = 7;
	else if (next.requests & (1U << 2)) next.action = 2;
	else if (next.requests & (1U << 4)) next.action = 6;
	else if (next.reset) next.action = 1;
	else if (next.drain) next.action = 4;
	else if (next.requests & (1U << 1)) next.action = 3;
	else next.action = 0;
	*state = next;
	return 0;
}
