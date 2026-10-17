// SPDX-License-Identifier: MIT
/* GA107 native host-execution adapter. Public MIT 615 TU102/GA102 workflow. */
#include <linux/firmware.h>
#include <linux/slab.h>
#include <core/device.h>
#include <engine/sec2.h>
#include <subdev/timer.h>
#include <rm/rm.h>
#include "../../priv.h"
#include "boot_payload.h"
#include "host_exec.h"
#include "pm.h"

struct r615_host {
	const struct firmware *loader;
	struct r615_loader_view view;
};

static int
r615_host_read(void *context, u32 reg, u32 *value)
{
	struct nvkm_gsp *gsp = context;

	*value = nvkm_falcon_rd32(&gsp->falcon, reg);
	return *value == ~0U ? -ENODEV : 0;
}

static int
r615_host_write(void *context, u32 reg, u32 value)
{
	struct nvkm_gsp *gsp = context;

	nvkm_falcon_wr32(&gsp->falcon, reg, value);
	return 0;
}

static int
r615_host_poll(void *context, u32 reg, u32 mask, u32 value)
{
	struct nvkm_gsp *gsp = context;
	bool removed = false;
	u32 actual;
	int ret = nvkm_msec(gsp->subdev.device, 2000,
		actual = nvkm_falcon_rd32(&gsp->falcon, reg);
		if (actual == ~0U) { removed = true; break; }
		if ((actual & mask) == value)
			break;
	);

	if (!removed && ret < 0)
		nvkm_error(&gsp->subdev, "r615: host poll reg=%03x mask=%08x want=%08x last=%08x timed out\n",
			   reg, mask, value, actual);
	return removed ? -ENODEV : ret < 0 ? -ETIMEDOUT : 0;
}

static int
r615_host_reset(void *context)
{
	struct nvkm_gsp *gsp = context;

	return nvkm_falcon_reset(&gsp->falcon);
}

static int
r615_host_memory(void *context, u32 space, u64 address, u64 bytes)
{
	struct nvkm_gsp *gsp = context;

	if (!bytes || address > ~0ULL - bytes)
		return -EINVAL;
	if (space == R615_HOST_FBMEM)
		return address <= gsp->fb.size && bytes <= gsp->fb.size - address ? 0 : -EINVAL;
	if (space == R615_HOST_SYSMEM)
		return r615_boot_payload_dma_owned(gsp->r615_payload, address, bytes) ||
		       r615_pm_dma_owned(gsp->r615_pm, address, bytes) ? 0 : -EACCES;
	return -EOPNOTSUPP;
}

static int
r615_host_resume(void *context)
{
	struct nvkm_gsp *gsp = context;
	struct nvkm_falcon *sec2 = &gsp->subdev.device->sec2->falcon;
	u64 libos;
	u32 version, control;
	bool removed = false;
	int ret;

	ret = r615_boot_payload_resume_info(gsp->r615_payload, &libos, &version);
	if (ret)
		return ret;
	ret = nvkm_falcon_get(sec2, &gsp->subdev);
	if (ret) {
		if (ret != -EBUSY)
			nvkm_falcon_put(sec2, &gsp->subdev);
		return ret;
	}
	ret = gsp->func->reset(gsp); /* switch back to authenticated RISC-V */
	if (ret)
		goto done;
	nvkm_falcon_wr32(&gsp->falcon, 0x040, lower_32_bits(libos));
	nvkm_falcon_wr32(&gsp->falcon, 0x044, upper_32_bits(libos));
	control = nvkm_falcon_rd32(sec2, 0x100);
	if (control == ~0U) {
		ret = -ENODEV;
		goto done;
	}
	/* SEC2 RTOS is already resident. Do not reset/reload it or reset queues. */
	nvkm_falcon_wr32(sec2, control & 0x40 ? 0x130 : 0x100, 2);
	ret = nvkm_msec(gsp->subdev.device, 2000,
		control = nvkm_rd32(gsp->subdev.device, 0x1180f8);
		if (control == ~0U) { removed = true; break; }
		if (control & 0x04000000)
			break;
	);
	if (removed || ret < 0) {
		ret = removed ? -ENODEV : -ETIMEDOUT;
		goto done;
	}
	control = nvkm_falcon_rd32(sec2, 0x040);
	if (control) {
		ret = control == ~0U ? -ENODEV : -EIO;
		goto done;
	}
	nvkm_falcon_wr32(&gsp->falcon, 0x080, version);
	removed = false;
	ret = nvkm_msec(gsp->subdev.device, 2000,
		/* GA107 public RISCV_CPUCTL ACTIVE_STAT. All ones is removal,
		 * not an active CPU (the generic boolean helper cannot distinguish it).
		 */
		control = nvkm_falcon_rd32(&gsp->falcon, 0x1388);
		if (control == ~0U) { removed = true; break; }
		if (control & 0x80)
			break;
	);
	ret = removed ? -ENODEV : ret < 0 ? -ETIMEDOUT : 0;
done:
	nvkm_falcon_put(sec2, &gsp->subdev);
	return ret;
}

static const struct r615_host_ops r615_native_host_ops = {
	.read = r615_host_read, .write = r615_host_write, .poll = r615_host_poll,
	.reset = r615_host_reset, .resume = r615_host_resume, .memory = r615_host_memory,
};

int
r615_host_new(struct nvkm_gsp *gsp)
{
	struct r615_host *host;
	int ret;

	if (!gsp || !gsp->subdev.device || gsp->r615_host || !gsp->rm ||
	    !gsp->rm->version || strcmp(gsp->rm->version, "615.78.08") ||
	    gsp->subdev.device->chipset != 0x177)
		return -EOPNOTSUPP;
	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;
	ret = request_firmware(&host->loader, "nvidia/ga107/gsp/sec2-bl-615.78.08.bin",
			       gsp->subdev.device->dev);
	if (!ret)
		ret = r615_loader_parse(host->loader->data, host->loader->size, &host->view);
	if (ret) {
		release_firmware(host->loader);
		kfree(host);
		return ret;
	}
	gsp->r615_host = host;
	return 0;
}

void
r615_host_del(struct nvkm_gsp *gsp)
{
	struct r615_host *host = gsp->r615_host;

	if (!host)
		return;
	release_firmware(host->loader);
	kfree(host);
	gsp->r615_host = NULL;
}

int
r615_host_command(struct nvkm_gsp *gsp, u32 command, const void *data, u32 bytes)
{
	struct r615_host_config config;
	int ret;

	if (!gsp->r615_host || !gsp->r615_payload || !gsp->subdev.device->sec2)
		return -EINVAL;
	ret = nvkm_falcon_get(&gsp->falcon, &gsp->subdev);
	if (ret) {
		if (ret != -EBUSY)
			nvkm_falcon_put(&gsp->falcon, &gsp->subdev);
		return ret;
	}
	config = (struct r615_host_config) {
		.imem_bytes = gsp->falcon.code.limit, .dmem_bytes = gsp->falcon.data.limit,
		.loader = gsp->r615_host->view,
	};
	ret = r615_host_execute(&config, &r615_native_host_ops, gsp, command, data, bytes);
	nvkm_falcon_put(&gsp->falcon, &gsp->subdev);
	return ret;
}

MODULE_FIRMWARE("nvidia/ga107/gsp/sec2-bl-615.78.08.bin");
