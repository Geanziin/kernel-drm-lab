/* SPDX-License-Identifier: MIT */
/* Public 615 interface excerpts: Copyright (c) 2008-2026 NVIDIA CORPORATION & AFFILIATES.
 * Queue layout: Copyright (c) 2015-2020 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. MIT permission notice: LICENSES/preferred/MIT.
 */
#ifndef __NVKM_R615_HEALTH_H__
#define __NVKM_R615_HEALTH_H__
#include <linux/types.h>

#define R615_GMMU_STATIC 0x20800a59U
#define R615_GMMU_REGISTER_SHADOW 0x20800a9dU
#define R615_GMMU_MAX_PAGES 3000U
#define R615_FAULT_BYTES 32U
#define R615_DISABLE_USER_CHANNELS 0x20801117U
#define R615_FECS_GET_ENABLE 0x20800a38U
#define R615_FECS_SET_ENABLE 0x20800a37U
#define R615_RESET_SCRATCH 0x1478U

struct r615_gmmu_static { u32 replay_bytes, replay_meta, fault_bytes, fault_meta; };
struct r615_shadow_register {
	u64 queue;
	u32 bytes, metadata_bytes;
	u64 pages[R615_GMMU_MAX_PAGES];
	u32 type;
	u64 shared;
};
/* Nonmanaged public Queue, x86_64/RV64 NvLength/pointer ABI. GET/PUT have
 * separate 64-byte cache lines. Neither native host pointer is published.
 */
struct r615_fault_queue {
	u64 capacity, allocator, data, message_bytes, padding0[4];
	u64 get, padding1[7];
	u64 put, padding2[7];
};
struct r615_fault_plan { u32 bytes, metadata_bytes, total_bytes, pages, capacity; };
struct r615_fault_record {
	u64 instance, address, timestamp;
	u16 engine;
	u8 aperture, type, access, client, gpc, hub;
};
struct r615_fecs_event { u32 gr; u8 type, padding[3]; };
struct r615_fecs_enable { u32 flags, padding; u64 route; u8 enable, reserved[7]; };
struct r615_recovery_state { u32 requests, action; bool reset, drain, contained; };

int r615_fault_plan(const struct r615_gmmu_static *, struct r615_fault_plan *);
int r615_fault_decode(const void *, struct r615_fault_record *);
int r615_recovery_update(struct r615_recovery_state *, u32 type, u8 value, u32 scratch);
struct nvkm_gsp;
struct r615_health;
int r615_health_start(struct nvkm_gsp *);
int r615_health_del(struct nvkm_gsp *);
int r615_health_faults(struct nvkm_gsp *, u32 budget);
int r615_health_fecs(struct nvkm_gsp *, const struct r615_fecs_event *);
int r615_health_recovery(struct nvkm_gsp *, u32 type, u8 value);
#endif
