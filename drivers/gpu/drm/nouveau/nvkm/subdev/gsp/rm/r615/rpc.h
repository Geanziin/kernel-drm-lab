/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Synchronous GMCAPI/VGPU RPC over r615_transport (MSGQ 2.0 + MCTP/NVDM).
 * Sleepable boot/worker context only. Serialized transactions and deferred
 * event dispatch; VGPU continuation records are bounded to 16 MiB.
 * Command IDs from public gmcapi_table.h (MIT, 615.78.08).
 */
#ifndef __NVKM_R615_RPC_H__
#define __NVKM_R615_RPC_H__

#include <linux/types.h>

struct r615_transport;
struct r615_rpc;
struct r615_message;
/* Called under the RPC lock: must copy borrowed data and must not issue RPCs. */
typedef int (*r615_rpc_dispatch_func)(void *, const struct r615_message *);

#define R615_GMCAPI_GSP_INIT 0x010001u
#define R615_GMCAPI_GSP_SUSPEND 0x010004u
#define R615_GMCAPI_GSP_RESUME_DONE 0x010005u
#define R615_SUSPEND_PM_TRANSITION (1ULL << 0)
#define R615_SUSPEND_HIBERNATE (1ULL << 2)
#define R615_RPC_CONTINUATION 71u
#define R615_RPC_MAX_BYTES (16U << 20)
#define R615_GMCAPI_MAX_SYSINFO_WORDS 8192u
#define R615_GMCAPI_MAX_STATIC_WORDS 6144u

int r615_rpc_new(struct r615_transport *transport, struct r615_rpc **output);
void r615_rpc_del(struct r615_rpc **rpc);
int r615_rpc_gsp_suspend(struct r615_rpc *rpc);
int r615_rpc_gsp_suspend_flags(struct r615_rpc *, u64 flags);
/* Arm before Booter resume, then drain retained queues. No GSP_INIT or reseed.
 * A timeout/invalid completion poisons the session; this is not recovery.
 */
int r615_rpc_resume_begin(struct r615_rpc *);
int r615_rpc_resume_wait(struct r615_rpc *, bool *d3hot);
/* Reserve internal control traffic for this PM task, serialized with in-flight
 * transactions. Does not bypass pm_pending, poison, or resume completion.
 */
int r615_rpc_pm_enter(struct r615_rpc *);
void r615_rpc_pm_exit(struct r615_rpc *);
void r615_rpc_invalidate(struct r615_rpc *);
void r615_rpc_set_dispatch(struct r615_rpc *, r615_rpc_dispatch_func, void *);
int r615_rpc_poll(struct r615_rpc *, u32 budget);
/* Native API allocation metadata; accepts only pointers returned by this API. */
u32 r615_rpc_api_bytes(const void *payload);

/* GMCAPI GSP_INIT: sysinfo in (NVKV words), static out (NVKV words).
 * Returns 0 with *static_count filled; caller decodes via r615_static_decode.
 */
int r615_rpc_gsp_init(struct r615_rpc *rpc, const u64 *sysinfo,
		      u32 sysinfo_count, u64 *static_info,
		      u32 static_limit, u32 *static_count);

/* Bounded VGPU RPC (type 0x25). Large bidirectional replies use request size. */
int r615_rpc_call(struct r615_rpc *rpc, u32 function,
		  const void *request, u32 req_bytes,
		  void *reply, u32 reply_limit, u32 *reply_bytes);

#endif
