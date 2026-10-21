/* SPDX-License-Identifier: MIT */
/* Interface excerpts: Copyright (c) 2008-2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. MIT permission notice: LICENSES/preferred/MIT.
 */
/* GSP-RM performance/power state (P-State) control, NV2080 top-level controls.
 * Definitions follow the public MIT 615 headers ctrl/ctrl2080/ctrl2080perf.h
 * and the proprietary driver's own OS layer (see perf.c for provenance).
 */
#ifndef __NVKM_GSP_RM_PERF_H__
#define __NVKM_GSP_RM_PERF_H__
#include <linux/types.h>

struct nvkm_gsp;

/* NV2080_CTRL_PERF_POWER_SOURCE_* */
#define NVKM_GSP_PERF_POWER_SOURCE_AC      0x00000000U
#define NVKM_GSP_PERF_POWER_SOURCE_BATTERY 0x00000001U

/* NV2080_CTRL_PERF_AUX_POWER_STATE_P0..P4: 0 is maximum performance (P0). */
#define NVKM_GSP_PERF_AUX_POWER_STATE_P0 0x00000000U
#define NVKM_GSP_PERF_AUX_POWER_STATE_P1 0x00000001U
#define NVKM_GSP_PERF_AUX_POWER_STATE_P2 0x00000002U
#define NVKM_GSP_PERF_AUX_POWER_STATE_P3 0x00000003U
#define NVKM_GSP_PERF_AUX_POWER_STATE_P4 0x00000004U

/* One-shot P0 setup: AC power source + maximum auxiliary power state, with a
 * current-P-State read-back.  Call once per GSP boot, after the internal
 * subdevice handle exists; never per client/device allocation.
 * Returns 0 when the P0 request was accepted by RM, negative errno otherwise
 * (the read-back is diagnostic: RM stays free to arbitrate the final level).
 */
int nvkm_gsp_perf_max_performance(struct nvkm_gsp *);
/* Read NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE (0x20802068).  Returns 0 and
 * stores the NV2080_CTRL_PERF_PSTATES_* bit for the current level.
 */
int nvkm_gsp_perf_current_pstate(struct nvkm_gsp *, u32 *);

#endif
