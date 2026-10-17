/* SPDX-License-Identifier: MIT */
/* Interface excerpts: Copyright (c) 2019-2025 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. MIT permission notice: LICENSES/preferred/MIT.
 */
/* Public 615 rmgspseq.h/rmflcnbl.h ABI, TU102/GA102 host execution contract. */
#ifndef __NVKM_R615_HOST_EXEC_H__
#define __NVKM_R615_HOST_EXEC_H__
#include <linux/types.h>

#define R615_HOST_GENERIC 0x010002U
#define R615_HOST_HS 0x010003U
#define R615_HOST_SYSMEM 1U
#define R615_HOST_FBMEM 2U

struct r615_bl_dmem {
	u32 reserved[4], signature[4], dma_index;
	struct { u32 lo, hi; } code;
	u32 nonsecure_offset, nonsecure_bytes, secure_offset, secure_bytes, entry;
	struct { u32 lo, hi; } data;
	u32 data_bytes, argc, argv;
};
struct r615_generic_request {
	struct r615_bl_dmem dmem;
	u32 descriptor_bytes, address_space, cache_attribute, reserved[4];
};
struct r615_hs_request {
	u64 imem_address, dmem_address, reserved64[2];
	u32 imem_va, imem_pa, imem_bytes, dmem_va, dmem_pa, dmem_bytes;
	u32 signature_dmem, engine_mask, ucode_id, reserved32[3];
};
struct r615_loader_view {
	const u8 *code;
	u32 code_bytes, start_tag;
};
struct r615_host_config {
	u32 imem_bytes, dmem_bytes;
	struct r615_loader_view loader;
};
struct r615_host_ops {
	int (*read)(void *, u32, u32 *);
	int (*write)(void *, u32, u32);
	int (*poll)(void *, u32, u32 mask, u32 value);
	int (*reset)(void *);
	int (*resume)(void *);
	/* Check the complete DMA source interval before the first hardware write. */
	int (*memory)(void *, u32 space, u64 address, u64 bytes);
};

/* Loader file: public RM_FLCN_BL_DESC (six LE u32s), followed by its image. */
int r615_loader_parse(const void *, size_t, struct r615_loader_view *);
int r615_host_execute(const struct r615_host_config *, const struct r615_host_ops *,
		      void *, u32 command, const void *request, u32 bytes);

struct nvkm_gsp;
int r615_host_new(struct nvkm_gsp *);
void r615_host_del(struct nvkm_gsp *);
int r615_host_command(struct nvkm_gsp *, u32, const void *, u32);
#endif
