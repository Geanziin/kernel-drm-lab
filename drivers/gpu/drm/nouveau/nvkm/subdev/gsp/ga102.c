/*
 * Copyright 2021 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "priv.h"

#include <rm/rm.h>

#include <nvfw/flcn.h>
#include <nvfw/fw.h>
#include <nvfw/hs.h>

int
ga102_gsp_reset(struct nvkm_gsp *gsp)
{
	int ret;

	ret = gsp->falcon.func->reset_eng(&gsp->falcon);
	if (ret)
		return ret;

	nvkm_falcon_mask(&gsp->falcon, 0x1668, 0x00000111, 0x00000111);
	return 0;
}

int
ga102_gsp_booter_ctor(struct nvkm_gsp *gsp, const char *name, const struct firmware *blob,
		      struct nvkm_falcon *falcon, struct nvkm_falcon_fw *fw)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	const struct nvkm_falcon_fw_func *func = &ga102_flcn_fw;
	const struct nvfw_bin_hdr *hdr;
	const struct nvfw_hs_header_v2 *hshdr;
	const struct nvfw_hs_load_header_v2 *lhdr;
	u32 loc, sig, cnt, *meta;
	int ret;

	hdr = nvfw_bin_hdr(subdev, blob->data);
	hshdr = nvfw_hs_header_v2(subdev, blob->data + hdr->header_offset);
	meta = (u32 *)(blob->data + hshdr->meta_data_offset);
	loc = *(u32 *)(blob->data + hshdr->patch_loc);
	sig = *(u32 *)(blob->data + hshdr->patch_sig);
	cnt = *(u32 *)(blob->data + hshdr->num_sig);

	ret = nvkm_falcon_fw_ctor(func, name, subdev->device, true,
				  blob->data + hdr->data_offset, hdr->data_size, falcon, fw);
	if (ret)
		goto done;

	ret = nvkm_falcon_fw_sign(fw, loc, hshdr->sig_prod_size / cnt, blob->data,
				  cnt, hshdr->sig_prod_offset + sig, 0, 0);
	if (ret)
		goto done;

	lhdr = nvfw_hs_load_header_v2(subdev, blob->data + hshdr->header_offset);

	fw->imem_base_img = lhdr->app[0].offset;
	fw->imem_base = 0;
	fw->imem_size = lhdr->app[0].size;

	fw->dmem_base_img = lhdr->os_data_offset;
	fw->dmem_base = 0;
	fw->dmem_size = lhdr->os_data_size;
	fw->dmem_sign = loc - lhdr->os_data_offset;

	fw->boot_addr = lhdr->app[0].offset;

	fw->fuse_ver = meta[0];
	fw->engine_id = meta[1];
	fw->ucode_id = meta[2];

done:
	if (ret)
		nvkm_falcon_fw_dtor(fw);

	return ret;
}

static int
ga102_gsp_fwsec_signature(struct nvkm_falcon_fw *fw, u32 *src_base_src)
{
	struct nvkm_falcon *falcon = fw->falcon;
	struct nvkm_device *device = falcon->owner->device;
	u32 sig_fuse_version = fw->fuse_ver;
	u32 reg_fuse_version;
	int idx = 0;

	FLCN_DBG(falcon, "brom: %08x %08x", fw->engine_id, fw->ucode_id);
	FLCN_DBG(falcon, "sig_fuse_version: %08x", sig_fuse_version);

	if (fw->engine_id & 0x00000400) {
		reg_fuse_version = nvkm_rd32(device, 0x8241c0 + (fw->ucode_id - 1) * 4);
	} else {
		WARN_ON(1);
		return -ENOSYS;
	}

	FLCN_DBG(falcon, "reg_fuse_version: %08x", reg_fuse_version);
	reg_fuse_version = BIT(fls(reg_fuse_version));
	FLCN_DBG(falcon, "reg_fuse_version: %08x", reg_fuse_version);
	if (!(reg_fuse_version & fw->fuse_ver))
		return -EINVAL;

	while (!(reg_fuse_version & sig_fuse_version & 1)) {
		idx += (sig_fuse_version & 1);
		reg_fuse_version >>= 1;
		sig_fuse_version >>= 1;
	}

	return idx;
}

const struct nvkm_falcon_fw_func
ga102_gsp_fwsec = {
	.signature = ga102_gsp_fwsec_signature,
	.reset = gm200_flcn_fw_reset,
	.load = ga102_flcn_fw_load,
	.boot = ga102_flcn_fw_boot,
};

const struct nvkm_falcon_func
ga102_gsp_flcn = {
	.disable = gm200_flcn_disable,
	.enable = gm200_flcn_enable,
	.select = ga102_flcn_select,
	.addr2 = 0x1000,
	.riscv_irqmask = 0x528,
	.reset_eng = gp102_flcn_reset_eng,
	.reset_prep = ga102_flcn_reset_prep,
	.reset_wait_mem_scrubbing = ga102_flcn_reset_wait_mem_scrubbing,
	.imem_dma = &ga102_flcn_dma,
	.dmem_dma = &ga102_flcn_dma,
	.riscv_active = ga102_flcn_riscv_active,
	.intr_retrigger = ga100_flcn_intr_retrigger,
};

static const struct nvkm_gsp_func
ga102_gsp_r535 = {
	.flcn = &ga102_gsp_flcn,
	.fwsec = &ga102_gsp_fwsec,

	.sig_section = ".fwsignature_ga10x",

	.booter.ctor = ga102_gsp_booter_ctor,

	.fwsec_sb.ctor = tu102_gsp_fwsec_sb_ctor,
	.fwsec_sb.dtor = tu102_gsp_fwsec_sb_dtor,

	.dtor = r535_gsp_dtor,
	.oneinit = tu102_gsp_oneinit,
	.init = tu102_gsp_init,
	.fini = tu102_gsp_fini,
	.reset = ga102_gsp_reset,

	.rm.gpu = &ga1xx_gpu,
};

static const struct nvkm_gsp_func
ga102_gsp_r615 = {
	.flcn = &ga102_gsp_flcn,
	.fwsec = &ga102_gsp_fwsec,

	.sig_section = ".fwsignature_ga10x",

	.booter.ctor = ga102_gsp_booter_ctor,

	.fwsec_sb.ctor = tu102_gsp_fwsec_sb_ctor,
	.fwsec_sb.dtor = tu102_gsp_fwsec_sb_dtor,

	.dtor = r615_gsp_dtor,
	.oneinit = r615_gsp_oneinit,
	.init = r615_gsp_init,
	.fini = r615_gsp_fini,
	.reset = ga102_gsp_reset,

	.rm.gpu = &ga1xx_gpu,
};

static const struct nvkm_gsp_func
ga102_gsp = {
	.flcn = &ga102_gsp_flcn,
};

static struct nvkm_gsp_fwif
ga102_gsps[] = {
	/* 615 requires its native lifecycle and framed MSGQ/GMCAPI transport.
	 * Single 615 entry: the 570-path hybrid (candidate 10) was removed so a
	 * version pin can only ever select the native backend.
	 */
	{  2, r615_gsp_load, &ga102_gsp_r615, &r615_rm_ga102, "615.78.08" },
	{  1, tu102_gsp_load, &ga102_gsp_r535, &r570_rm_ga102, "570.144" },
	{  0, tu102_gsp_load, &ga102_gsp_r535, &r535_rm_ga102, "535.113.01" },
	{ -1, gv100_gsp_nofw, &ga102_gsp },
	{}
};

#ifdef NOUVEAU_LAB_PIN_GSP_615
static char *gsp_fw_ver = "615.78.08";
#else
static char *gsp_fw_ver;
#endif
module_param(gsp_fw_ver, charp, 0444);
MODULE_PARM_DESC(gsp_fw_ver,
		 "GSP firmware version to try first, e.g. 615.78.08, 570.144; "
		 "experimental 615 requires an explicit pin");

/*
 * Build the probe order: the requested version first, everything else in its
 * default order. Lets a boot test pin one version from the kernel command line
 * without rebuilding, while keeping the rest of the table as fallbacks.
 */
static int
ga102_gsp_fwif_order(struct nvkm_gsp_fwif *selection, unsigned capacity,
		     const struct nvkm_gsp_fwif *defaults)
{
	unsigned count = 0;
	bool pinned = false;

	if (capacity < ARRAY_SIZE(ga102_gsps))
		return -ENOSPC;
	if (gsp_fw_ver && gsp_fw_ver[0]) {
		for (unsigned i = 0; defaults[i].load; i++) {
			if (!defaults[i].ver || strcmp(defaults[i].ver, gsp_fw_ver))
				continue;
			selection[count++] = defaults[i];
			pinned = true;
			break;
		}
		if (!pinned)
			return -EINVAL;
	}
	/* The no-firmware entry has ver == NULL but still has a load callback. */
	for (unsigned i = 0; defaults[i].load && count < capacity - 1; i++) {
		/* Experimental 615 is opt-in, never an implicit promotion or fallback. */
		if (defaults[i].ver && !strcmp(defaults[i].ver, "615.78.08"))
			continue;
		if (pinned && defaults[i].ver && !strcmp(defaults[i].ver, gsp_fw_ver))
			continue;
		selection[count++] = defaults[i];
	}
	selection[count] = (struct nvkm_gsp_fwif){};
	return 0;
}

int
ga102_gsp_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	      struct nvkm_gsp **pgsp)
{
	struct nvkm_gsp_fwif selection[ARRAY_SIZE(ga102_gsps)];
	int ret;

	ret = ga102_gsp_fwif_order(selection, ARRAY_SIZE(selection), ga102_gsps);
	if (ret)
		return ret;
	return nvkm_gsp_new_(selection, device, type, inst, pgsp);
}

NVKM_GSP_FIRMWARE_BOOTER(ga102, 535.113.01);
NVKM_GSP_FIRMWARE_BOOTER(ga103, 535.113.01);
NVKM_GSP_FIRMWARE_BOOTER(ga104, 535.113.01);
NVKM_GSP_FIRMWARE_BOOTER(ga106, 535.113.01);
NVKM_GSP_FIRMWARE_BOOTER(ga107, 535.113.01);

NVKM_GSP_FIRMWARE_BOOTER(ga102, 570.144);
NVKM_GSP_FIRMWARE_BOOTER(ga103, 570.144);
NVKM_GSP_FIRMWARE_BOOTER(ga104, 570.144);
NVKM_GSP_FIRMWARE_BOOTER(ga106, 570.144);
NVKM_GSP_FIRMWARE_BOOTER(ga107, 570.144);

NVKM_GSP_FIRMWARE_BOOTER(ga102, 615.78.08);
NVKM_GSP_FIRMWARE_BOOTER(ga103, 615.78.08);
NVKM_GSP_FIRMWARE_BOOTER(ga104, 615.78.08);
NVKM_GSP_FIRMWARE_BOOTER(ga106, 615.78.08);
NVKM_GSP_FIRMWARE_BOOTER(ga107, 615.78.08);
