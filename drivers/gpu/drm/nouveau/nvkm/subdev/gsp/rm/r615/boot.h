/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_BOOT_H__
#define __NVKM_R615_BOOT_H__
#include <linux/types.h>

#define R615_INIT_MAGIC 0x20505347U
#define R615_INIT_PM_TRANSITION 1ULL
#define R615_INIT_DMEM_STACK 2ULL

/* Public 615 kernel_gsp.c:kgspAllocRmStateMonitorBuffer allocates one
 * contiguous 4 KiB system-memory page, independently of LOGRMON.
 */
#define R615_STATE_MONITOR_BYTES 4096U

struct r615_queue_args {
	u64 flags;
	u64 shared_mem_pa;
	u64 cmd_offset;
	u64 status_offset;
	u32 element_header_bytes;
	u32 element_min_bytes;
	u32 element_max_bytes;
	u32 header_alignment_log2;
	u32 element_alignment_log2;
	u32 pte_count;
	u8 reserved[8];
};

struct r615_init_args {
	u32 magic;
	u16 size;
	u8 reserved[2];
	u64 flags;
	struct r615_queue_args queue;
	struct {
		u32 old_level;
		u32 flags;
		u8 reserved[8];
	} resume;
	u8 reserved2[4];
	u32 gpu_instance;
	struct { u64 address, size; } profiler, sysmem_heap, state_monitor, bindata, vbios;
};

struct r615_boot_config {
	struct r615_queue_args queue;
	u64 shared_mem_bytes;
	u32 gpu_instance;
	bool resume;
	bool dmem_stack;
	u32 old_level;
	u32 pm_flags;
};

/* Serializes validated queue metadata, but neither allocates memory nor boots GSP. */
int r615_build_init_args(const struct r615_boot_config *, struct r615_init_args *);

#endif
