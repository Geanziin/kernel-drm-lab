/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_RUSD_H__
#define __NVKM_R615_RUSD_H__
#include <linux/types.h>

#define R615_RUSD_SIZE                 3184
#define R615_RUSD_PMA_OFFSET           16
#define R615_RUSD_CLOCK_OFFSET         72
#define R615_RUSD_UTIL_OFFSET          1992
#define R615_RUSD_POWER_LIMIT_OFFSET   2296
#define R615_RUSD_TEMPERATURE_OFFSET   2312
#define R615_RUSD_AVG_POWER_OFFSET     2472
#define R615_RUSD_INST_POWER_OFFSET    2496
#define R615_RUSD_POWER_POLICY_OFFSET  2520
#define R615_RUSD_PCIE_OFFSET          2536
#define R615_RUSD_FAN_OFFSET           2584
#define R615_RUSD_SEQ_START            0xff00000000000000ULL
#define R615_RUSD_READ_ATTEMPTS        10

enum r615_rusd_metric {
	R615_RUSD_GPU_BUSY,
	R615_RUSD_MEMORY_BUSY,
	R615_RUSD_ENCODER_BUSY,
	R615_RUSD_DECODER_BUSY,
	R615_RUSD_GPU_CLOCK_HZ,
	R615_RUSD_MEMORY_CLOCK_HZ,
	R615_RUSD_GPU_TEMP_MC,
	R615_RUSD_MEMORY_TEMP_MC,
	R615_RUSD_BOARD_TEMP_MC,
	R615_RUSD_GPU_POWER_AVG_UW,
	R615_RUSD_GPU_POWER_INST_UW,
	R615_RUSD_GPU_POWER_LIMIT_REQUESTED_UW,
	R615_RUSD_GPU_POWER_LIMIT_ENFORCED_UW,
	R615_RUSD_PMA_TOTAL_BYTES,
	R615_RUSD_PMA_FREE_BYTES,
	R615_RUSD_FAN0_RPM,
};

/* The caller pins a validated 615 mapping and supplies coherent device reads. */
struct r615_rusd_source {
	void *context;
	size_t bytes;
	int (*copy)(void *context, size_t offset, void *destination, size_t bytes);
	void (*read_barrier)(void *context);
};

struct r615_rusd_value {
	s64 value;
	u64 timestamp;
	u32 sampling_period_us;
};

int r615_rusd_read(const struct r615_rusd_source *, enum r615_rusd_metric,
		   u64 now_ns, u64 max_age_ns, struct r615_rusd_value *);

#endif
