// SPDX-License-Identifier: MIT
/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * r615 RM impl for GA107 (615.78.08). Native WPR/transport/CTRL/ALLOC.
 * Engine adapters still require per-schema 615 validation.
 */
#include <rm/rm.h>

/* Validated against 615.78.08 public sources
 * (src/nvidia/inc/kernel/gpu/gsp/gsp_fw_heap.h,
 *  commit 1a82fbfda8f321b056eb3568850f9316c5e465c5):
 * OS 22M baremetal LibOS3, base 8M Turing-Ampere, min 88M.
 * Numerically same as r570_wpr_libos3; re-derived from 615, not copied.
 */
static const struct nvkm_rm_wpr
r615_wpr_libos3 = {
	.os_carveout_size = (22 << 20),
	.base_size = (8 << 20),
	.heap_size_min = 88,
};

static const struct nvkm_rm_api
r615_api = {
	.gsp = &r615_gsp,
	.rpc = &r615_rpc_api,
	.ctrl = &r615_ctrl,
	.alloc = &r615_alloc,
	.client = &r570_client,
	.device = &r535_device,
	/* Native lifecycle owns FBSR and propagates resume errors (void legacy
	 * fbsr.resume cannot represent them). Never dispatch to a legacy adapter.
	 */
	.disp = &r615_disp,
	.fifo = &r615_fifo,
	.ce = &r535_ce,
	.gr = &r570_gr,
	.nvdec = &r535_nvdec,
	.nvenc = &r535_nvenc,
	.nvjpg = &r535_nvjpg,
	.ofa = &r570_ofa,
	.therm = &r615_therm,
	.telemetry = &r615_telemetry_api,
};

const struct nvkm_rm_impl
r615_rm_ga102 = {
	.wpr = &r615_wpr_libos3,
	.api = &r615_api,
};
