// SPDX-License-Identifier: MIT
/* CBC interface excerpt: Copyright (c) 2004-2017 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. MIT permission notice: LICENSES/preferred/MIT.
 * Native GA107 system-SR integration from public 615 contracts. No GC6.
 * The nouveau device PM owner serializes these sleepable operations and freezes
 * clients/instmem lists. RPC ownership also excludes background control callers.
 */
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/suspend.h>
#include <core/device.h>
#include <subdev/bar.h>
#include <subdev/instmem/priv.h>
#include <subdev/mmu/vmm.h>
#include <subdev/timer.h>
#include <rm/rm.h>
#include "boot_payload.h"
#include "dma_image.h"
#include "fbsr.h"
#include "pm.h"
#include "rpc.h"
#include "runtime.h"
#include "sr.h"

/* System-SR has never executed on hardware. The cold-boot diagnostic must not
 * reach it implicitly through a suspend request; an explicit opt-in is needed.
 */
static bool r615_pm_sr;
module_param_named(r615_pm, r615_pm_sr, bool, 0400);
MODULE_PARM_DESC(r615_pm, "r615: allow experimental system suspend/resume (default off)");

struct r615_pm {
	struct r615_sr *sr;
	struct r615_dma_image *backup;
	struct nvkm_gsp_object memlist;
	u64 *pages, sr_address;
	struct r615_pm_plan plan;
	bool exposed, failed, sleeping, ready, copies;
};

/* Public ctrl0080fb.h, NV0080_CTRL_FB_GET_COMPBIT_STORE_INFO_PARAMS. */
struct r615_pm_cbc {
	u64 size, address;
	u32 space, max_line, tags_per_line, line_bytes, slice_bytes, fetch_alignment;
	u64 backing_base;
	u32 gobs_per_tag, cbc_base, policy;
	u64 private_offset;
	u32 coverage;
	u64 region_bytes;
};

static int
r615_pm_allocate(struct nvkm_gsp *gsp, struct r615_pm **output)
{
	struct r615_wpr_meta wpr;
	struct r615_pm_cbc *cbc;
	struct r615_pm *pm;
	u64 cbc_bytes;
	int ret;

	if (!gsp->r615_static || !gsp->subdev.device->imem || !gsp->subdev.device->bar ||
	    !nvkm_bar_bar2_vmm(gsp->subdev.device) || !nvkm_bar_bar1_vmm(gsp->subdev.device))
		return -EOPNOTSUPP;
	ret = r615_boot_payload_wpr(gsp->r615_payload, &wpr);
	if (ret)
		return ret;
	/* Include CBC conservatively even if the RM preservation policy would
	 * skip it. Unknown CBC information must not silently under-size backup.
	 */
	cbc = nvkm_gsp_rm_ctrl_rd(&gsp->internal.device.object, 0x801306U, sizeof(*cbc));
	if (IS_ERR(cbc))
		return PTR_ERR(cbc);
	cbc_bytes = cbc->region_bytes;
	nvkm_gsp_rm_ctrl_done(&gsp->internal.device.object, cbc);
	if (READ_ONCE(gsp->r615_recovery_required) || !r615_runtime_pm_clean(gsp))
		return -EBUSY;
	pm = kzalloc_obj(*pm, GFP_KERNEL);
	if (!pm)
		return -ENOMEM;
	ret = r615_pm_plan(&wpr, gsp->r615_static, cbc_bytes, &pm->plan);
	if (ret)
		goto fail;
	ret = r615_sr_new(gsp->subdev.device->dev, &wpr, &pm->sr);
	if (ret)
		goto fail;
	ret = r615_dma_image_zero_new(gsp->subdev.device->dev, pm->plan.backup_bytes, &pm->backup);
	if (ret)
		goto fail;
	pm->pages = kvcalloc(pm->plan.pages, sizeof(*pm->pages), GFP_KERNEL);
	if (!pm->pages) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = r615_dma_image_pages(pm->backup, pm->pages, pm->plan.pages);
	if (ret)
		goto fail;
	*output = pm;
	return 0;
fail:
	r615_sr_del(&pm->sr);
	r615_dma_image_del(&pm->backup);
	kvfree(pm->pages);
	kfree(pm);
	return ret;
}

static int
r615_pm_save_instances(struct nvkm_gsp *gsp)
{
	struct nvkm_instmem *imem = gsp->subdev.device->imem;
	struct nvkm_instobj *object;
	int ret;

	/* Do not overwrite another owner's saved copies on retry. */
	list_for_each_entry(object, &imem->boot, head)
		if (object->suspend)
			return -EBUSY;
	list_for_each_entry(object, &imem->list, head)
		if (object->suspend)
			return -EBUSY;
	gsp->r615_pm->copies = true;
	list_for_each_entry(object, &imem->list, head) {
		if (object->preserve) {
			ret = nvkm_instobj_save(object);
			if (ret)
				return ret;
		}
	}
	/* Boot objects include BAR2's own page tables. Snapshot them through the
	 * BAR0 slowpath; mapping them through BAR2 could allocate/change tables
	 * while taking the snapshot. Firmware owns the hardware BAR transition.
	 */
	gsp->subdev.device->bar->bar2 = false;
	list_for_each_entry(object, &imem->boot, head) {
		ret = nvkm_instobj_save(object);
		if (ret)
			return ret;
	}
	return 0;
}

int
r615_pm_del(struct nvkm_gsp *gsp)
{
	struct r615_pm *pm = gsp->r615_pm;

	if (!pm)
		return 0;
	/* On removal other subdevices/lists may already have been destroyed.
	 * Never walk instmem here. Copies are consumed only by successful resume;
	 * an interrupted restore retains this owner instead of touching stale lists.
	 */
	if (pm->copies)
		return -EBUSY;
	/* GSP reset alone is NOT proof that a failed FBSR/CE operation stopped. */
	if (pm->exposed && (pm->failed || (!pm->ready && !pm->sleeping)))
		return -EBUSY;
	if (pm->sleeping && !gsp->r615_stopped)
		return -EBUSY;
	r615_sr_quiesced(pm->sr);
	r615_sr_del(&pm->sr);
	r615_dma_image_quiesced(pm->backup);
	r615_dma_image_del(&pm->backup);
	kvfree(pm->pages);
	kfree(pm);
	gsp->r615_pm = NULL;
	return 0;
}

bool
r615_pm_sleeping(const struct r615_pm *pm)
{
	return pm && pm->sleeping && !pm->failed;
}

bool
r615_pm_dma_owned(const struct r615_pm *pm, u64 address, u64 bytes)
{
	return pm && !pm->failed && (r615_sr_dma_owned(pm->sr, address, bytes) ||
				    r615_dma_image_owns(pm->backup, address, bytes));
}

int
r615_pm_suspend(struct nvkm_gsp *gsp)
{
	struct r615_pm *pm;
	u64 root, bytes;
	u32 mailbox = 0;
	bool submitted, removed = false;
	int ret;

	/* NVKM_SUSPEND alone also covers hibernation freeze/poweroff in nouveau.
	 * Do not advertise those as the level-3 RAM-suspend path: hibernation's
	 * level-7/image ownership and thaw/restore split are not integrated yet.
	 */
	if (!r615_pm_sr || pm_suspend_target_state == PM_SUSPEND_ON)
		return -EOPNOTSUPP;
	if (READ_ONCE(gsp->r615_recovery_required))
		return -EIO;
	if (gsp->r615_pm || !gsp->running || !gsp->r615_rpc || gsp->r615_suspend_sent)
		return -EBUSY;
	/* Complete deferred health controls before excluding other RPC tasks.
	 * Otherwise cancel_work_sync could wait for a worker whose next control
	 * was just refused by our private-task gate.
	 */
	ret = r615_runtime_pm_pause(gsp);
	if (ret)
		return ret;
	if (READ_ONCE(gsp->r615_recovery_required) || !r615_runtime_pm_clean(gsp)) {
		r615_runtime_pm_resume(gsp);
		return -EBUSY;
	}
	ret = r615_rpc_pm_enter(gsp->r615_rpc);
	if (ret) {
		r615_runtime_pm_resume(gsp);
		return ret;
	}
	ret = r615_pm_allocate(gsp, &pm);
	if (ret) {
		int restart;

		r615_rpc_pm_exit(gsp->r615_rpc);
		restart = r615_runtime_pm_resume(gsp);
		if (restart) {
			gsp->r615_suspend_sent = true;
			r615_rpc_invalidate(gsp->r615_rpc);
		}
		return ret; /* no GPU PM mutation or externally exposed new memory */
	}
	gsp->r615_pm = pm;
	gsp->r615_suspend_sent = true; /* external API gate; PM task has private access */
	ret = r615_fbsr_channels(gsp, true);
	if (ret)
		goto fail;
	ret = r615_pm_save_instances(gsp);
	if (ret)
		goto fail;
	ret = r615_sr_publish(pm->sr, &pm->sr_address);
	if (ret)
		goto fail;
	ret = r615_dma_image_expose(pm->backup, &root, &bytes);
	if (ret)
		goto fail;
	/* Conservatively include registration errors as possible DMA exposure. */
	pm->exposed = true;
	ret = r615_fbsr_memlist(&gsp->internal.device, 0xcaf00003, R615_MEMLIST_SYSMEM,
			       pm->plan.backup_bytes, pm->pages, pm->plan.pages, &pm->memlist, &submitted);
	if (ret)
		goto fail;
	ret = r615_fbsr_init(gsp, gsp->internal.client.object.handle, pm->memlist.handle,
			    pm->sr_address, &submitted);
	if (ret)
		goto fail;
	ret = r615_fbsr_memlist_free(&pm->memlist);
	if (ret)
		goto fail;
	if (READ_ONCE(gsp->r615_recovery_required) || !r615_runtime_pm_clean(gsp)) {
		ret = -EBUSY;
		goto fail;
	}
	ret = r615_rpc_gsp_suspend_flags(gsp->r615_rpc, R615_SUSPEND_PM_TRANSITION);
	if (ret)
		goto fail;
	ret = nvkm_msec(gsp->subdev.device, 2000,
		mailbox = nvkm_falcon_rd32(&gsp->falcon, 0x040);
		if (mailbox == ~0U) { removed = true; break; }
		if (mailbox & 0x80000000)
			break;
	);
	if (removed || ret < 0) {
		ret = removed ? -ENODEV : -ETIMEDOUT;
		goto fail;
	}
	gsp->r615_suspended = true;
	ret = r615_gsp_teardown_pm(gsp, pm->sr_address);
	if (ret)
		goto fail;
	ret = r615_boot_payload_pm_pause(gsp->r615_payload);
	if (ret)
		goto fail;
	r615_sr_quiesced(pm->sr);
	pm->sleeping = true;
	r615_rpc_pm_exit(gsp->r615_rpc);
	return 0;
fail:
	pm->failed = true;
	nvkm_error(&gsp->subdev, "r615: SR/FBSR suspend failed (%d), PM owner retained\n", ret);
	r615_rpc_invalidate(gsp->r615_rpc);
	r615_rpc_pm_exit(gsp->r615_rpc);
	return ret;
}

int
r615_pm_resume(struct nvkm_gsp *gsp)
{
	struct r615_pm *pm = gsp->r615_pm;
	struct nvkm_device *device = gsp->subdev.device;
	struct nvkm_instobj *object;
	struct nvkm_vmm *vmm;
	struct r615_boot_handoff handoff;
	bool d3hot;
	int ret;

	if (!r615_pm_sleeping(pm) || !gsp->r615_stopped || !gsp->r615_rpc)
		return -EIO;
	ret = r615_rpc_pm_enter(gsp->r615_rpc);
	if (ret)
		return ret;
	ret = r615_rpc_resume_begin(gsp->r615_rpc);
	if (ret)
		goto fail;
	ret = r615_sr_publish(pm->sr, &pm->sr_address);
	if (ret)
		goto fail;
	ret = r615_boot_payload_pm_resume(gsp->r615_payload, 3, &handoff);
	if (ret)
		goto fail;
	pm->sleeping = false;
	ret = r615_gsp_bootstrap_pm(gsp, pm->sr_address, handoff.app_version);
	if (ret)
		goto fail;
	ret = r615_rpc_resume_wait(gsp->r615_rpc, &d3hot);
	if (ret)
		goto fail;
	if (READ_ONCE(gsp->r615_recovery_required) || !r615_runtime_pm_clean(gsp)) {
		ret = -EBUSY;
		goto fail;
	}
	nvkm_debug(&gsp->subdev, "r615: resume complete, D3HOT=%d\n", d3hot);
	/* Firmware has completed FBSR restore. Rebuild CPU-visible BAR tables
	 * before allowing other nouveau engines or clients to access BAR2.
	 */
	gsp->running = true;
	/* Booter/FWSEC and the power transition may have changed the BAR0
	 * aperture. GA107 uses nv50 instmem: invalidate its cached window before
	 * restoring the boot objects by slowpath, not only during pre-SR fini.
	 */
	nv50_instmem_fini(device->imem);
	list_for_each_entry(object, &device->imem->boot, head)
		if (object->suspend)
			nvkm_instobj_load(object);
	device->bar->bar2 = true;
	vmm = nvkm_bar_bar2_vmm(device);
	vmm->func->flush(vmm, 0);
	list_for_each_entry(object, &device->imem->list, head)
		if (object->suspend)
			nvkm_instobj_load(object);
	vmm = nvkm_bar_bar1_vmm(device);
	vmm->func->flush(vmm, 0);
	pm->copies = false;
	ret = r615_fbsr_channels(gsp, false);
	if (ret)
		goto fail;
	pm->ready = true;
	ret = r615_pm_del(gsp);
	if (ret)
		goto fail;
	gsp->r615_suspend_sent = false;
	gsp->r615_suspended = false;
	r615_rpc_pm_exit(gsp->r615_rpc);
	ret = r615_runtime_pm_resume(gsp);
	if (ret) {
		gsp->r615_suspend_sent = true;
		r615_rpc_invalidate(gsp->r615_rpc);
	}
	return ret;
fail:
	pm->failed = true;
	nvkm_error(&gsp->subdev, "r615: SR/FBSR resume failed (%d), PM owner retained\n", ret);
	r615_rpc_invalidate(gsp->r615_rpc);
	r615_rpc_pm_exit(gsp->r615_rpc);
	return ret;
}
