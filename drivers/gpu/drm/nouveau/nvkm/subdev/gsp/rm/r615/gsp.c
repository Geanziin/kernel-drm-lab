// SPDX-License-Identifier: MIT
/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * r615 GSP-RM ops for GA107 (615.78.08). Native cold-boot handoff,
 * NVKV static info and engine-ID integration. Runtime lives in runtime.c;
 * inherited engine adapters remain subject to per-schema validation.
 */
#include <linux/build_bug.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include <asm-generic/video.h>

#include <core/device.h>
#include <core/pci.h>
#include <subdev/pci/priv.h>
#include <rm/rm.h>

#include "boot.h"
#include "bootmem.h"
#include "sysinfo.h"

/* Validated against 615.78.08 public sources:
 * gsp_fw_heap.h (OS 22M, RM 8M TU10x, min 88M baremetal),
 * gsp_init_args.h (MESSAGE_QUEUE_INIT_ARGUMENTS / GSP_ARGUMENTS_CACHED),
 * gsp_fw_wpr_meta_v1.h (256B V1).
 */

static bool
r615_gsp_compatible(struct nvkm_gsp *gsp)
{
	return gsp && gsp->rm && gsp->rm->version &&
	       !strcmp(gsp->rm->version, "615.78.08") &&
	       gsp->subdev.device &&
	       gsp->subdev.device->chipset == 0x177;
}

static u32
r615_gsp_sr_data_size(struct nvkm_gsp *gsp)
{
	struct r615_wpr_meta *meta = gsp->wpr_meta.data;

	return (meta->frts_start + meta->frts_bytes) -
	       (meta->non_wpr_start + meta->non_wpr_bytes);
}

static bool
r615_gsp_xlat_mc_engine_idx(u32 mc_engine_idx, enum nvkm_subdev_type *ptype, int *pinst)
{
	/* Public 615 gpu/intr/engine_idx.h. This is an ID mapping, independent
	 * of queue layout and of the backend used for control transport.
	 */
	if (mc_engine_idx >= 15 && mc_engine_idx <= 34) {
		*ptype = NVKM_ENGINE_CE;
		*pinst = mc_engine_idx - 15;
	} else if (mc_engine_idx >= 38 && mc_engine_idx <= 41) {
		*ptype = NVKM_ENGINE_NVENC;
		*pinst = mc_engine_idx - 38;
	} else if (mc_engine_idx >= 51 && mc_engine_idx <= 58) {
		*ptype = NVKM_ENGINE_NVJPG;
		*pinst = mc_engine_idx - 51;
	} else if (mc_engine_idx >= 65 && mc_engine_idx <= 72) {
		*ptype = NVKM_ENGINE_NVDEC;
		*pinst = mc_engine_idx - 65;
	} else if (mc_engine_idx == 81 || mc_engine_idx == 82) {
		*ptype = NVKM_ENGINE_OFA;
		*pinst = mc_engine_idx - 81;
	} else {
		*pinst = 0;
		switch (mc_engine_idx) {
		case 2: *ptype = NVKM_ENGINE_DISP; break;
		case 50: *ptype = NVKM_SUBDEV_GSP; break;
		case 84: *ptype = NVKM_ENGINE_GR; break;
		default: return false;
		}
	}
	return true;
}

/* Legacy API hook, not the native boot producer. Native cold boot uses
 * boot_payload.c's allocation-to-RMARGS chain (including bindata/monitor).
 * SR and callers expecting a complete handoff from this hook are unsupported.
 */
static void
r615_gsp_set_rmargs(struct nvkm_gsp *gsp, bool resume)
{
	struct r615_init_args *args = gsp->rmargs.data;
	struct r615_boot_config config = { 0 };

	if (!r615_gsp_compatible(gsp) || !args)
		return;

	config.queue.flags = 0;
	config.queue.shared_mem_pa = gsp->shm.mem.addr;
	config.queue.cmd_offset =
		(u8 *)gsp->shm.cmdq.ptr - (u8 *)gsp->shm.mem.data;
	config.queue.status_offset =
		(u8 *)gsp->shm.msgq.ptr - (u8 *)gsp->shm.mem.data;
	config.queue.element_header_bytes = 24;
	config.queue.element_min_bytes = 4096;
	config.queue.element_max_bytes = 65536;
	config.queue.header_alignment_log2 = 4;
	config.queue.element_alignment_log2 = 12;
	config.queue.pte_count = gsp->shm.ptes.nr;
	config.shared_mem_bytes = gsp->shm.ptes.size +
				  gsp->shm.cmdq.size +
				  gsp->shm.msgq.size;
	config.gpu_instance = 0;
	config.dmem_stack = true;
	if (resume) {
		config.resume = true;
		config.old_level = 3; /* LEVEL_3, matches r570 resume */
		config.pm_flags = 0; /* TODO: GPU_STATE_FLAGS_* for 615 */
	}

	if (r615_build_init_args(&config, args))
		memset(args, 0, sizeof(*args));
	/* bindata.address/size stay zero until Stage C wires boot_payload. */
}

static int
r615_gsp_set_system_info(struct nvkm_gsp *gsp)
{
	struct nvkm_device *device = gsp->subdev.device;
	struct pci_dev *pdev;
	struct r615_sysinfo info = { 0 };
	u64 *words;
	u32 count;
	int ret;

	if (!r615_gsp_compatible(gsp))
		return -EOPNOTSUPP;
	if (device->type == NVKM_DEVICE_TEGRA)
		return -ENOSYS;
	pdev = container_of(device, struct nvkm_device_pci, device)->pdev;

	info.pci_device = (pdev->device << 16) | pdev->vendor;
	info.pci_sub = (pdev->subsystem_device << 16) | pdev->subsystem_vendor;
	info.pci_rev = pdev->revision;
	info.phys_bar0 = device->func->resource_addr(device, NVKM_BAR0_PRI);
	info.phys_fb = device->func->resource_addr(device, NVKM_BAR1_FB);
	info.phys_inst = device->func->resource_addr(device, NVKM_BAR2_INST);
	info.domain_bdf = (u64)pci_domain_nr(pdev->bus) << 32 |
			  (u64)pdev->bus->number << 8 | PCI_SLOT(pdev->devfn);
	info.cfg_base = device->pci->func->cfg.addr;
	info.cfg_size = device->pci->func->cfg.size;
	info.primary = video_is_primary_device(device->dev);
	info.console_mem = 0;
	info.max_user_va = TASK_SIZE;

	words = kvcalloc(R615_SYSINFO_MAX_WORDS, sizeof(u64), GFP_KERNEL);
	if (!words)
		return -ENOMEM;
	ret = r615_sysinfo_encode(&info, words, R615_SYSINFO_MAX_WORDS, &count);
	if (ret) {
		kvfree(words);
		return ret;
	}
	kvfree(gsp->r615_sysinfo);
	gsp->r615_sysinfo = words;
	gsp->r615_sysinfo_count = count;
	return 0;
}

/* Public 615 ctrl2080gpu.h: NV2080_CTRL_GPU_GET_FERMI_{GPC,TPC}_INFO. Local
 * 615 wire definitions instead of the r570 adapter's headers, so this backend
 * does not depend on another release's ABI (layouts pinned by the checks).
 */
#define R615_GPU_GET_FERMI_GPC_INFO 0x20800137U
#define R615_GPU_GET_FERMI_TPC_INFO 0x20800138U
struct r615_gpc_info { u32 gpc_mask; };
struct r615_tpc_info { u32 gpc, tpc_mask; };
static_assert(sizeof(struct r615_gpc_info) == 4);
static_assert(sizeof(struct r615_tpc_info) == 8);

static int
r615_gr_gpc_mask(struct nvkm_gsp *gsp, u32 *mask)
{
	struct r615_gpc_info *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_rd(&gsp->internal.device.subdevice,
				   R615_GPU_GET_FERMI_GPC_INFO, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);
	*mask = ctrl->gpc_mask;
	nvkm_gsp_rm_ctrl_done(&gsp->internal.device.subdevice, ctrl);
	return 0;
}

static int
r615_gr_tpc_mask(struct nvkm_gsp *gsp, int gpc, u32 *mask)
{
	struct r615_tpc_info *ctrl;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&gsp->internal.device.subdevice,
				    R615_GPU_GET_FERMI_TPC_INFO, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);
	ctrl->gpc = gpc;
	ret = nvkm_gsp_rm_ctrl_push(&gsp->internal.device.subdevice, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&gsp->internal.device.subdevice, ctrl);
		return ret;
	}
	*mask = ctrl->tpc_mask;
	nvkm_gsp_rm_ctrl_done(&gsp->internal.device.subdevice, ctrl);
	return 0;
}

static int
r615_gsp_get_static_info(struct nvkm_gsp *gsp)
{
	struct r615_static *cached = gsp->r615_static;
	u32 gpc_mask, tpc_mask;
	int ret;

	if (!r615_gsp_compatible(gsp) || !cached)
		return -EOPNOTSUPP;

	gsp->internal.client.object.client = &gsp->internal.client;
	gsp->internal.client.object.parent = NULL;
	gsp->internal.client.object.handle = cached->client;
	gsp->internal.client.gsp = gsp;
	INIT_LIST_HEAD(&gsp->internal.client.events);

	gsp->internal.device.object.client = &gsp->internal.client;
	gsp->internal.device.object.parent = &gsp->internal.client.object;
	gsp->internal.device.object.handle = cached->device;

	gsp->internal.device.subdevice.client = &gsp->internal.client;
	gsp->internal.device.subdevice.parent = &gsp->internal.device.object;
	gsp->internal.device.subdevice.handle = cached->subdevice;

	gsp->bar.rm_bar1_pdb = cached->bar1_pdb;
	gsp->bar.rm_bar2_pdb = cached->bar2_pdb;

	gsp->fb.region_nr = 0;
	for (u32 i = 0; i < cached->region_count; i++) {
		u64 base = cached->regions[i].base;
		u64 limit = cached->regions[i].limit;

		if (!r615_fb_region_usable(&cached->regions[i]))
			continue;
		if (gsp->fb.region_nr >= ARRAY_SIZE(gsp->fb.region))
			return -ENOSPC;
		gsp->fb.region[gsp->fb.region_nr].addr = base;
		gsp->fb.region[gsp->fb.region_nr].size = (limit + 1) - base;
		gsp->fb.region_nr++;
	}
	if (!gsp->fb.region_nr)
		return -ENOENT;

	ret = r615_gr_gpc_mask(gsp, &gpc_mask);
	if (ret)
		return ret;
	for (int gpc = 0; gpc < 32; gpc++) {
		if (gpc_mask & BIT(gpc)) {
			ret = r615_gr_tpc_mask(gsp, gpc, &tpc_mask);
			if (ret)
				return ret;
			gsp->gr.tpcs += hweight32(tpc_mask);
			gsp->gr.gpcs++;
		}
	}
	return 0;
}

const struct nvkm_rm_api_gsp r615_gsp = {
	.set_rmargs = r615_gsp_set_rmargs,
	.set_system_info = r615_gsp_set_system_info,
	.get_static_info = r615_gsp_get_static_info,
	.xlat_mc_engine_idx = r615_gsp_xlat_mc_engine_idx,
	.sr_data_size = r615_gsp_sr_data_size,
};
