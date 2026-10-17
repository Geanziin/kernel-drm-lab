/* SPDX-License-Identifier: MIT */
/* NOCAT interface excerpt: Copyright (c) 2004-2023 NVIDIA CORPORATION & AFFILIATES.
 * Recovery interface excerpt: Copyright (c) 2008-2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. MIT permission notice: LICENSES/preferred/MIT.
 */
/* Wire facts from public MIT 615 ctrl2080internal.h / g_rpc-structures.h. */
#ifndef __NVKM_R615_RUNTIME_H__
#define __NVKM_R615_RUNTIME_H__
#include <linux/types.h>

#define R615_INTR_GET_TABLE 0x20800a5cU
#define R615_INTR_ENTRIES 128U
struct r615_intr_entry {
	u16 engine, reserved;
	u32 mask, stall, nonstall;
};
struct r615_intr_table {
	u32 count;
	struct r615_intr_entry entries[R615_INTR_ENTRIES];
	u32 padding;
	u64 subtree[7];
};

#define R615_EVENT_POST 0x1003U
#define R615_EVENT_RC 0x1004U
#define R615_EVENT_MMU 0x1005U
#define R615_EVENT_ERROR 0x1006U
#define R615_EVENT_LIBOS_PRINT 0x100cU
#define R615_EVENT_LOCKDOWN 0x101cU
#define R615_EVENT_NOCAT 0x1020U
#define R615_EVENT_FORCED_SHUTDOWN 0x1025U
#define R615_EVENT_PMU_HALTED 0x1029U
#define R615_EVENT_RECOVERY 0x1022U
#define R615_EVENT_FECS 0x1021U
#define R615_EVENT_BUGCHECK 0x1023U
/* Retained public enum IDs without public 615 handlers/schema. Do not route
 * these through legacy registrations. EXEC_* now uses native GMCAPI.
 */
#define R615_EVENT_OLD_INIT 0x1001U
#define R615_EVENT_OLD_SEQUENCER 0x1002U
#define R615_EVENT_OLD_GENERIC 0x1026U
#define R615_EVENT_OLD_HS 0x1027U
#define R615_GMCAPI_MMU_FAULT_QUEUED 0x040001U
struct r615_recovery_event { u32 type; u8 value, padding[3]; };
/* Public ctrl2080nvd.h record starts at rpc_gsp_post_nocat_record.data itself,
 * not after its placeholder u32. Fixed strings may be non-NUL-terminated.
 */
struct r615_nocat_event {
	u32 flags;
	u64 timestamp;
	u8 type;
	u32 bugcheck;
	char source[65];
	u32 subsystem;
	u64 error;
	char engine[65];
	u32 tdr, diagnostic_bytes;
	u8 diagnostic[1024];
};
struct r615_post_event {
	u32 client, event, index, data;
	u16 info, padding;
	u32 status, bytes;
	u8 notify_list;
	u8 payload[];
};
struct r615_error_event { u32 xid, previous_xid, runlist, chid; char text[256]; };
struct r615_rc_event {
	u32 engine, chid, gfid, level, type, scope;
	u16 partition, padding;
	u32 fault_lo, fault_hi, fault_type;
	u8 callback_needed, padding2[3];
	u32 info_type, inst_lo, inst_hi, aperture, journal_bytes;
	u8 journal[];
};

int r615_intr_table_validate(const struct r615_intr_table *, u32 max_vector);
int r615_post_event_validate(const void *, u32);
int r615_event_validate(u32 function, const void *, u32);
bool r615_event_native(u32 function);
struct nvkm_gsp;
int r615_runtime_new(struct nvkm_gsp *);
int r615_runtime_start(struct nvkm_gsp *);
void r615_runtime_stop(struct nvkm_gsp *);
int r615_runtime_pm_pause(struct nvkm_gsp *);
int r615_runtime_pm_resume(struct nvkm_gsp *);
bool r615_runtime_pm_clean(struct nvkm_gsp *);
/* Count of public-enum events without a nouveau consumer, logged and dropped. */
u64 r615_runtime_dropped(struct nvkm_gsp *);
void r615_runtime_del(struct nvkm_gsp *);
/* Independent CPU copy under the runtime lock; never returns borrowed memory. */
int r615_runtime_log_copy(struct nvkm_gsp *, void *, u32 limit, u32 *bytes);
#endif
