/* SPDX-License-Identifier: MIT */
#include <linux/errno.h>
#include <linux/string.h>

#include "rusd.h"

static u32
r615_rusd_u32(const u8 *bytes)
{
	return (u32)bytes[0] | (u32)bytes[1] << 8 |
	       (u32)bytes[2] << 16 | (u32)bytes[3] << 24;
}

static u64
r615_rusd_u64(const u8 *bytes)
{
	return r615_rusd_u32(bytes) | (u64)r615_rusd_u32(bytes + 4) << 32;
}

static int
r615_rusd_snapshot(const struct r615_rusd_source *source, size_t offset,
		   size_t bytes, u64 now_ns, u64 max_age_ns, u8 *snapshot)
{
	u8 stamp[8];
	u64 before, after;
	int ret, attempt;

	if (!source || !source->copy || !source->read_barrier ||
	    source->bytes < R615_RUSD_SIZE || bytes < sizeof(stamp) ||
	    offset > source->bytes || bytes > source->bytes - offset)
		return -EINVAL;

	for (attempt = 0; attempt < R615_RUSD_READ_ATTEMPTS; attempt++) {
		ret = source->copy(source->context, offset, stamp, sizeof(stamp));
		if (ret)
			return ret;
		before = r615_rusd_u64(stamp);
		if (!before)
			return -ENODATA;
		if (before >= R615_RUSD_SEQ_START && (before & 1))
			continue;

		source->read_barrier(source->context);
		ret = source->copy(source->context, offset, snapshot, bytes);
		if (ret)
			return ret;
		source->read_barrier(source->context);
		ret = source->copy(source->context, offset, stamp, sizeof(stamp));
		if (ret)
			return ret;
		after = r615_rusd_u64(stamp);
		if (before != after || before != r615_rusd_u64(snapshot))
			continue;
		if (before < R615_RUSD_SEQ_START) {
			/* now_ns must use the producer's verified timestamp domain. */
			if (!max_age_ns || before > now_ns)
				return -ESTALE;
			if (now_ns - before > max_age_ns)
				return -ESTALE;
		}
		return 0;
	}

	return -EAGAIN;
}

int
r615_rusd_read(const struct r615_rusd_source *source, enum r615_rusd_metric metric,
	       u64 now_ns, u64 max_age_ns, struct r615_rusd_value *output)
{
	struct r615_rusd_value result = { 0 };
	u8 data[48];
	size_t offset, bytes = 16, field = 8;
	u32 raw;
	int ret;

	if (!output)
		return -EINVAL;
	switch (metric) {
	case R615_RUSD_GPU_BUSY:
	case R615_RUSD_MEMORY_BUSY:
	case R615_RUSD_ENCODER_BUSY:
	case R615_RUSD_DECODER_BUSY:
		offset = R615_RUSD_UTIL_OFFSET;
		bytes = 48;
		field = 8 + (metric - R615_RUSD_GPU_BUSY) * 4;
		if (metric == R615_RUSD_DECODER_BUSY)
			field = 24;
		break;
	case R615_RUSD_GPU_CLOCK_HZ:
	case R615_RUSD_MEMORY_CLOCK_HZ:
		offset = R615_RUSD_CLOCK_OFFSET;
		bytes = 40;
		field = metric == R615_RUSD_GPU_CLOCK_HZ ? 12 : 20;
		break;
	case R615_RUSD_GPU_TEMP_MC:
	case R615_RUSD_MEMORY_TEMP_MC:
	case R615_RUSD_BOARD_TEMP_MC:
		offset = R615_RUSD_TEMPERATURE_OFFSET +
			 (metric - R615_RUSD_GPU_TEMP_MC) * 16;
		break;
	case R615_RUSD_GPU_POWER_AVG_UW:
		offset = R615_RUSD_AVG_POWER_OFFSET;
		bytes = 24;
		break;
	case R615_RUSD_GPU_POWER_INST_UW:
		offset = R615_RUSD_INST_POWER_OFFSET;
		bytes = 24;
		break;
	case R615_RUSD_GPU_POWER_LIMIT_REQUESTED_UW:
	case R615_RUSD_GPU_POWER_LIMIT_ENFORCED_UW:
		offset = R615_RUSD_POWER_LIMIT_OFFSET;
		field = metric == R615_RUSD_GPU_POWER_LIMIT_REQUESTED_UW ? 8 : 12;
		break;
	case R615_RUSD_PMA_TOTAL_BYTES:
	case R615_RUSD_PMA_FREE_BYTES:
		offset = R615_RUSD_PMA_OFFSET;
		bytes = 24;
		field = metric == R615_RUSD_PMA_TOTAL_BYTES ? 8 : 16;
		break;
	case R615_RUSD_FAN0_RPM:
		offset = R615_RUSD_FAN_OFFSET;
		/* Only timestamp and the first cooler are needed. */
		break;
	default:
		return -EINVAL;
	}

	ret = r615_rusd_snapshot(source, offset, bytes, now_ns, max_age_ns, data);
	if (ret)
		return ret;
	result.timestamp = r615_rusd_u64(data);
	if (metric == R615_RUSD_PMA_TOTAL_BYTES || metric == R615_RUSD_PMA_FREE_BYTES) {
		u64 total = r615_rusd_u64(data + 8);
		u64 free = r615_rusd_u64(data + 16);

		if (free > total || total > 0x7fffffffffffffffULL)
			return -ERANGE;
		result.value = metric == R615_RUSD_PMA_TOTAL_BYTES ? total : free;
	} else {
		raw = r615_rusd_u32(data + field);
		if (metric <= R615_RUSD_DECODER_BUSY) {
			if (raw > 100)
				return -ERANGE;
			result.value = raw;
			if (metric == R615_RUSD_ENCODER_BUSY || metric == R615_RUSD_DECODER_BUSY) {
				result.sampling_period_us = r615_rusd_u32(data + field + 4);
				if (!result.sampling_period_us)
					return -ENODATA;
			}
		} else if (metric >= R615_RUSD_GPU_TEMP_MC && metric <= R615_RUSD_BOARD_TEMP_MC) {
			s32 fixed = (s32)raw;

			if (fixed < -273 * 256 || fixed > 1000 * 256)
				return -ERANGE;
			result.value = (s64)fixed * 1000 / 256;
		} else if (metric == R615_RUSD_FAN0_RPM) {
			result.value = raw;
		} else {
			/* Actual clocks are kHz; power quantities are mW. */
			result.value = (u64)raw * 1000;
		}
	}
	*output = result;
	return 0;
}
