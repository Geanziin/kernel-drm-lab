/* SPDX-License-Identifier: MIT */
/* Interface excerpts: Copyright (c) 2008-2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. MIT permission notice: LICENSES/preferred/MIT.
 * Public 615 rpc_alloc_memory_v13_01 / pte_desc / ctrl2080internal.h.
 */
#ifndef __NVKM_R615_FBSR_H__
#define __NVKM_R615_FBSR_H__
#include <linux/types.h>

#define R615_ALLOC_MEMORY 4U
#define R615_FBSR_INIT 0x20800ac2U
#define R615_FBSR_CHANNELS 0x20800ac3U
#define R615_MEMLIST_SYSMEM 1U
#define R615_MEMLIST_VRAM 2U
struct r615_memlist {
	u32 client, device, memory, class, flags, pte_adjust, format, reserved;
	u64 bytes;
	u32 page_count, reserved2, descriptor, reserved3;
	u64 pfn[];
};
struct r615_memlist_config {
	u32 client, device, memory, space, page_count;
	u64 bytes;
	const u64 *pages; /* DMA byte addresses; one entry for contiguous VRAM. */
};
struct r615_fbsr_init {
	u32 client, sysmem;
	u8 gcoff;
	u64 sr_metadata;
};
int r615_memlist_size(const struct r615_memlist_config *, u32 *request_bytes);
/* out must not alias config/pages. Validation completes before any write. */
int r615_memlist_build(const struct r615_memlist_config *, void *out, u32 capacity);

struct nvkm_gsp;
struct nvkm_gsp_device;
struct nvkm_gsp_object;
/* submitted is set BEFORE handing the request to transport. On any subsequent
 * error, caller must retain backing DMA until registration revoked or GPU
 * stopped. FBSR_INIT creates a separate exposure: its references must survive
 * until restoration completes or GPU stop is proved, even after object free.
 */
int r615_fbsr_memlist(struct nvkm_gsp_device *, u32 memory, u32 space, u64 bytes,
		      const u64 *pages, u32 page_count, struct nvkm_gsp_object *, bool *submitted);
/* Retain handle/owner on failure. Success alone does not revoke FBSR_INIT refs. */
int r615_fbsr_memlist_free(struct nvkm_gsp_object *);
int r615_fbsr_init(struct nvkm_gsp *, u32 client, u32 sysmem, u64 sr_metadata, bool *submitted);
int r615_fbsr_channels(struct nvkm_gsp *, bool suspend);
#endif
