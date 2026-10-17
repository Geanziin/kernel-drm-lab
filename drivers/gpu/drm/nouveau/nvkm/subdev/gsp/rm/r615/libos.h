/* SPDX-License-Identifier: MIT */
/* Wire excerpt: Copyright (c) 2008-2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. MIT permission notice: LICENSES/preferred/MIT.
 * Public 615 g_rpc-structures.h / g_eng_desc_nvoc.h / g_kern_pmu_nvoc.h.
 */
#ifndef __NVKM_R615_LIBOS_H__
#define __NVKM_R615_LIBOS_H__
#include <linux/types.h>

#define R615_LIBOS_PMU_ENGINE 0xf3d72200U
#define R615_LIBOS_MAX_BYTES 4096U
struct r615_libos_event { u32 engine, bytes; u8 data[]; };
/* CPU-owned last complete RPC snapshot. The caller serializes access.
 * PMU does not advertise PACKED_METADATA in public 615: these are raw words,
 * not strings or decoded records. Never dereference embedded firmware VAs.
 */
struct r615_libos_capture {
	u64 events, put;
	u32 bytes;
	u8 data[R615_LIBOS_MAX_BYTES];
};
int r615_libos_validate(const void *, u32);
int r615_libos_update(struct r615_libos_capture *, const void *, u32);
int r615_libos_copy(const struct r615_libos_capture *, void *, u32 limit, u32 *bytes);
#endif
