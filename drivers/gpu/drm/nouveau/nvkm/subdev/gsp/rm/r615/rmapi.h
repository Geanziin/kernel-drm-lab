/* SPDX-License-Identifier: MIT */
/* Interface excerpts: Copyright (c) 2008-2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. MIT permission notice: LICENSES/preferred/MIT.
 */
/* Public MIT 615 g_rpc-structures.h, rpc_global_enums.h, nvstatuscodes.h. */
#ifndef __NVKM_R615_RMAPI_H__
#define __NVKM_R615_RMAPI_H__
#include <linux/types.h>

#define R615_RM_CONTROL 76U
#define R615_RM_ALLOC 103U
#define R615_RM_FREE 10U

struct r615_rm_control {
	u32 client, object, command, status, params_bytes;
	u32 rpc_flags, control_flags, access_right;
	u64 reserved;
	u8 params[];
};
struct r615_rm_alloc {
	u32 client, parent, object, class, status, params_bytes, flags;
	u8 reserved[4];
	u8 params[];
};
struct r615_rm_free {
	u32 client, parent, object, status;
};
int r615_rm_status(u32 status);
/* Wire size for an inherited allocator request (append-only 615 extensions). */
u32 r615_alloc_wire_bytes(u32 class, u32 bytes);
u32 r615_ctrl_wire_bytes(u32 command, u32 bytes);
#endif
