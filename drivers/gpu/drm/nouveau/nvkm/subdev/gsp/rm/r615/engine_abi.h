/* SPDX-License-Identifier: MIT */
/* Interface excerpts: Copyright (c) 2005-2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. MIT permission notice: LICENSES/preferred/MIT.
 */
/* Public MIT 615 alloc_channel.h, ctrl2080internal.h, ctrl0073{dp,system}.h. */
#ifndef __NVKM_R615_ENGINE_ABI_H__
#define __NVKM_R615_ENGINE_ABI_H__
#include <linux/types.h>

struct r615_memory_desc { u64 base, bytes; u32 space, cache; };
struct r615_channel_alloc {
	u32 error, buffer;
	u64 gpfifo;
	u32 entries, flags, context_share, vaspace, handle_vaspace, userd[8];
	u64 userd_offset[8];
	u32 engine, cid, subdevice, ecc_error;
	struct r615_memory_desc instance, userd_mem, ramfc, methods;
	u32 group, internal_flags;
	struct r615_memory_desc error_mem, ecc_mem;
	u32 pid, subpid, encrypt_iv[3], decrypt_iv[3], hmac_nonce[8], tpc_config;
};
struct r615_channel_config {
	u64 gpfifo, instance, userd, methods;
	u32 gpfifo_bytes, instance_bytes, userd_bytes, methods_bytes;
	u32 vaspace, engine, chid, runqueue;
	bool privileged;
};
int r615_channel_build(const struct r615_channel_config *, struct r615_channel_alloc *);

#define R615_DISPLAY_STATIC 0x20800a01U
#define R615_DISPLAY_ACTIVE 0x73010cU
#define R615_DP_CAPS 0x731369U
struct r615_display_static {
	u32 system_cap, windows, heads, i2c, active, embedded_dp;
	u8 external_mux, internal_mux;
	u32 channels;
};
struct r615_display_active { u32 subdevice, head, flags, display, stale_sor; };
struct r615_dsc_caps { u8 supported; u32 color_format, line_kb, rate_kb, precision, slices, depth; };
struct r615_dp_caps {
	u32 subdevice, sor, link_rate, versions, uhbr;
	u8 mst, sc, watermark, pc2_disabled, single_head_mst, fec, train_repeater;
	u8 override_bw, rg_flush, downspread, avoid_hbr3, polling_mst, tunnel_war, internal_tunnel;
	struct r615_dsc_caps dsc;
};

/* Public 615 ctrl2080fifo.h GET_DEVICE_INFO_TABLE: ENGINE_DATA_TYPES grew to
 * 17 (IS_PREEMPTABLE, engine_info.h). Field indices 0-14 unchanged.
 */
#define R615_FIFO_DEVICE_INFO 0x20801112U
#define R615_FIFO_DEVICE_ENTRIES 32U
#define R615_FIFO_ENGINE_DATA 17U
enum {
	R615_ENGINE_INFO_ENG_DESC = 0, R615_ENGINE_INFO_RM_ENGINE_TYPE = 2,
	R615_ENGINE_INFO_RUNLIST = 3, R615_ENGINE_INFO_RUNLIST_PRI_BASE = 11,
};
struct r615_fifo_device_entry {
	u32 data[R615_FIFO_ENGINE_DATA], pbdma[2], pbdma_fault[2], pbdmas;
	char name[16];
};
struct r615_fifo_device_info {
	u32 base, count;
	u8 more, padding[3];
	struct r615_fifo_device_entry entries[R615_FIFO_DEVICE_ENTRIES];
};
#endif
