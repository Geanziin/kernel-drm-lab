// SPDX-License-Identifier: MIT
/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * r615 GA107 lifecycle (oneinit/init/fini) for 615.78.08.
 * Native cold boot and retained system-SR; runtime PM remains unsupported.
 * Uses r615_boot_payload + MSGQ 2.0 transport + GMCAPI GSP_INIT.
 * Reuses Booter/FWSEC/reset and GR/disp/fifo engines where wire unchanged.
 */
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <core/device.h>
#include <engine/sec2.h>
#include <subdev/fb.h>
#include <subdev/timer.h>

#include <rm/rm.h>
#include <rm/rpc.h>

#include "../../priv.h"

#include "lifecycle.h"

#include "boot_payload.h"
#include "bootmem.h"
#include "rpc.h"
#include "runtime.h"
#include "sysinfo.h"
#include "transport.h"
#include "../perf.h"
#include "host_exec.h"
#include "pm.h"
#include "health.h"
#include "telemetry.h"

/* Deliberate reboot-lifetime retention after an unsuccessful hardware stop.
 * There is no deferred reclaim against a removed nvkm_device or loaded code.
 */
static LIST_HEAD(r615_quarantine);
static DEFINE_MUTEX(r615_quarantine_lock);

static int
r615_gsp_teardown_mode(struct nvkm_gsp *gsp, u64 sr_metadata)
{
	int ret;

	if (gsp->r615_teardown_stage < 1) {
		ret = nvkm_falcon_reset(&gsp->falcon);
		if (ret)
			return ret;
		gsp->r615_teardown_stage = 1;
	}
	if (gsp->r615_teardown_stage < 2) {
		ret = nvkm_gsp_fwsec_sb(gsp);
		if (ret)
			return ret;
		gsp->r615_teardown_stage = 2;
	}
	/* SR needs Booter to write its saved state, not just WPR2-down. */
	if (sr_metadata && !nvkm_rd32(gsp->subdev.device, 0x1fa828))
		return -EIO;
	if (nvkm_rd32(gsp->subdev.device, 0x1fa828)) {
		u32 mbox0 = sr_metadata ? lower_32_bits(sr_metadata) : 0xff;
		u32 mbox1 = sr_metadata ? upper_32_bits(sr_metadata) : 0xff;

		ret = nvkm_falcon_fw_boot(&gsp->booter.unload, &gsp->subdev,
					  true, &mbox0, &mbox1, 0, 0);
		if (ret)
			return ret;
		if (nvkm_rd32(gsp->subdev.device, 0x1fa828))
			return -EIO;
	}
	gsp->r615_teardown_stage = 3;
	gsp->running = false;
	gsp->r615_stopped = true;
	return 0;
}

static int
r615_gsp_teardown(struct nvkm_gsp *gsp)
{
	return r615_gsp_teardown_mode(gsp, 0);
}

int
r615_gsp_teardown_pm(struct nvkm_gsp *gsp, u64 sr_metadata)
{
	if (!sr_metadata || sr_metadata % R615_BOOT_PAGE || sr_metadata > (1ULL << 49) - R615_BOOT_PAGE)
		return -EINVAL;
	return r615_gsp_teardown_mode(gsp, sr_metadata);
}

static bool
r615_lifecycle_compatible(struct nvkm_gsp *gsp)
{
	return gsp && gsp->rm && gsp->rm->version &&
	       !strcmp(gsp->rm->version, "615.78.08") &&
	       gsp->subdev.device &&
	       gsp->subdev.device->chipset == 0x177;
}

int
r615_gsp_load(struct nvkm_gsp *gsp, int ver, const struct nvkm_gsp_fwif *fwif)
{
	int ret;

	(void)ver;
	if (gsp->subdev.device->chipset != 0x177)
		return -EOPNOTSUPP;
	nvkm_info(&gsp->subdev, "r615: trying 615.78.08 firmware set\n");
	ret = tu102_gsp_load(gsp, ver, fwif);
	nvkm_info(&gsp->subdev, "r615: firmware set load %s (%d)\n",
		  ret ? "FAILED, falling back" : "ok", ret);
	return ret;
}

static u64
r615_vga_workspace(struct nvkm_gsp *gsp, u64 fb_size)
{
	struct nvkm_device *device = gsp->subdev.device;
	const u64 base = fb_size - 0x100000;
	u64 addr = 0;

	if (device->disp)
		addr = nvkm_rd32(device, 0x625f04);
	if (!(addr & 0x00000008))
		return base;
	addr = (addr & 0xffffff00) << 8;
	if (addr < base)
		return fb_size - 0x20000;
	return addr;
}

static int
r615_quiesce(struct nvkm_gsp *gsp, void *context)
{
	(void)context;
	if (!gsp)
		return -EINVAL;
	/* A software running flag alone is not proof that DMA stopped. */
	return !gsp->r615_stopped || gsp->running ? -EBUSY : 0;
}

/* Snapshot GSP/RM progress after a failed init, before teardown resets it.
 * Distinguishes: Booter never ran (WPR2 down) vs RM alive but silent
 * (WPR2 up, INIT log has text) vs queues half-published (heads nonzero).
 */
static void
r615_init_debug_dump(struct nvkm_gsp *gsp)
{
	struct nvkm_device *device = gsp->subdev.device;
	const void *log;
	size_t log_bytes = 0;

	nvkm_info(&gsp->subdev,
		  "r615: state riscv=%d mbox0=%08x mbox1=%08x wpr2=%08x%08x\n",
		  !!nvkm_falcon_riscv_active(&gsp->falcon),
		  nvkm_falcon_rd32(&gsp->falcon, 0x040),
		  nvkm_falcon_rd32(&gsp->falcon, 0x044),
		  nvkm_rd32(device, 0x1fa828), nvkm_rd32(device, 0x1fa824));
	nvkm_info(&gsp->subdev,
		  "r615: msgq txhead=%08x txtail=%08x rxhead=%08x rxtail=%08x\n",
		  nvkm_rd32(device, 0x110c00), nvkm_rd32(device, 0x110c04),
		  nvkm_rd32(device, 0x110c80), nvkm_rd32(device, 0x110c84));
	if (gsp->r615_payload &&
	    (log = r615_boot_payload_log_init(gsp->r615_payload, &log_bytes)) &&
	    log_bytes >= 64) {
		const u32 *w = log;

		nvkm_info(&gsp->subdev,
			  "r615: initlog %08x %08x %08x %08x %08x %08x %08x %08x\n",
			  w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
		nvkm_info(&gsp->subdev,
			  "r615: initlog %08x %08x %08x %08x %08x %08x %08x %08x\n",
			  w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
	}
}

/* Rebuild all externally published addresses after a completed poweroff.
 * Firmware objects/locks belong to oneinit; payload/queues belong to each boot.
 */
static int
r615_gsp_stage(struct nvkm_gsp *gsp)
{
	struct r615_boot_environment env = { 0 };
	struct r615_wpr_meta wpr = { 0 };
	int ret;

	if (gsp->running || gsp->r615_payload || gsp->r615_rpc || gsp->r615_runtime ||
	    gsp->r615_health || gsp->r615_pm ||
	    gsp->r615_quarantined)
		return -EBUSY;
	/* A cold rebuild cannot restore external RM handles. Reject it while
	 * clients survive; the SR path must preserve/restore them separately.
	 */
	mutex_lock(&gsp->client_id.mutex);
	ret = idr_is_empty(&gsp->client_id.idr) ? 0 : -EBUSY;
	mutex_unlock(&gsp->client_id.mutex);
	if (ret)
		return ret;
	env.fb_bytes = gsp->fb.size;
	env.vga_start = gsp->fb.bios.vga_workspace.addr;
	env.vga_valid = true;
	env.mmu_lock_valid = false;
	env.boost = true;
	ret = r615_boot_payload_request(gsp, &env, r615_quiesce, NULL,
					&gsp->r615_payload);
	if (ret) {
		nvkm_info(&gsp->subdev, "r615: boot payload request failed (%d)\n", ret);
		return ret;
	}

	ret = r615_boot_payload_wpr(gsp->r615_payload, &wpr);
	if (ret)
		goto dtor_payload;

	gsp->fb.wpr2.frts.addr = wpr.frts_start;
	gsp->fb.wpr2.frts.size = wpr.frts_bytes;
	gsp->fb.wpr2.boot.addr = wpr.boot_start;
	gsp->fb.wpr2.boot.size = wpr.boot_bytes;
	gsp->fb.wpr2.elf.addr = wpr.elf_start;
	gsp->fb.wpr2.elf.size = wpr.elf_bytes;
	gsp->fb.wpr2.heap.addr = wpr.heap_start;
	gsp->fb.wpr2.heap.size = wpr.heap_bytes;
	gsp->fb.wpr2.addr = wpr.wpr_start;
	gsp->fb.wpr2.size = wpr.wpr_end - wpr.wpr_start;
	gsp->fb.heap.addr = wpr.non_wpr_start;
	gsp->fb.heap.size = wpr.non_wpr_bytes;

	if (!gsp->wpr_meta.data) {
		ret = nvkm_gsp_mem_ctor(gsp, sizeof(wpr), &gsp->wpr_meta);
		if (ret)
			goto dtor_payload;
	}
	memcpy(gsp->wpr_meta.data, &wpr, sizeof(wpr));

	if (gsp->fb.wpr2.frts.size) {
		ret = nvkm_gsp_fwsec_frts(gsp);
		if (WARN_ON(ret))
			goto dtor_wpr_meta;
	}

	ret = gsp->func->reset(gsp);
	if (ret)
		goto dtor_wpr_meta;
	gsp->gr.gpcs = 0;
	gsp->gr.tpcs = 0;
	/* Only this fresh, handle-empty boot may retire the old recovery latch. */
	WRITE_ONCE(gsp->r615_recovery_required, false);
	return 0;

dtor_wpr_meta:
	nvkm_gsp_mem_dtor(&gsp->wpr_meta);
dtor_payload:
	r615_boot_payload_del(&gsp->r615_payload);
	return ret;
}

int
r615_gsp_oneinit(struct nvkm_gsp *gsp)
{
	struct nvkm_device *device;
	int ret;

	if (!r615_lifecycle_compatible(gsp))
		return -EOPNOTSUPP;
	device = gsp->subdev.device;
	gsp->fb.size = nvkm_fb_vidmem_size(device);
	if (gsp->fb.size < 0x100000)
		return -EINVAL;
	gsp->fb.bios.vga_workspace.addr = r615_vga_workspace(gsp, gsp->fb.size);
	gsp->fb.bios.vga_workspace.size = gsp->fb.size - gsp->fb.bios.vga_workspace.addr;
	gsp->fb.bios.addr = gsp->fb.bios.vga_workspace.addr;
	gsp->fb.bios.size = gsp->fb.bios.vga_workspace.size;
	ret = gsp->func->booter.ctor(gsp, "booter-load", gsp->fws.booter.load,
				    &device->sec2->falcon, &gsp->booter.load);
	if (ret)
		return ret;
	ret = gsp->func->booter.ctor(gsp, "booter-unload", gsp->fws.booter.unload,
				    &device->sec2->falcon, &gsp->booter.unload);
	if (ret)
		goto dtor_booter_load;
	mutex_init(&gsp->cmdq.mutex);
	mutex_init(&gsp->msgq.mutex);
	mutex_init(&gsp->client_id.mutex);
	idr_init(&gsp->client_id.idr);
	gsp->r615_locks_ready = true;
	ret = nvkm_gsp_fwsec_sb_ctor(gsp);
	if (ret)
		goto dtor_client_id;
	gsp->r615_fwsec_ready = true;
	ret = r615_host_new(gsp);
	if (ret)
		goto dtor_fwsec_sb;
	ret = gsp->rm->api->gsp->set_system_info(gsp);
	if (!ret)
		ret = r615_gsp_stage(gsp);
	if (!ret) {
		nvkm_info(&gsp->subdev, "r615: oneinit ok, payload staged\n");
		return 0;
	}
	kvfree(gsp->r615_sysinfo);
	gsp->r615_sysinfo = NULL;
	gsp->r615_sysinfo_count = 0;
	r615_host_del(gsp);
dtor_fwsec_sb:
	nvkm_gsp_fwsec_sb_dtor(gsp);
	gsp->r615_fwsec_ready = false;
dtor_client_id:
	idr_destroy(&gsp->client_id.idr);
	mutex_destroy(&gsp->client_id.mutex);
	mutex_destroy(&gsp->msgq.mutex);
	mutex_destroy(&gsp->cmdq.mutex);
	gsp->r615_locks_ready = false;
	nvkm_falcon_fw_dtor(&gsp->booter.unload);
dtor_booter_load:
	nvkm_falcon_fw_dtor(&gsp->booter.load);
	return ret;
}

/* 615 TU102 bootstrap HAL also covers GA107. GSP and SEC2 are distinct
 * Falcons: GSP consumes LibOS regions; SEC2 Booter consumes WPR V1 metadata.
 * nvkm_falcon_fw_boot() checks the executing Falcon's mailbox status.
 */
static int
r615_gsp_bootstrap(struct nvkm_gsp *gsp, const struct r615_boot_handoff *handoff)
{
	u32 mbox0 = lower_32_bits(handoff->wpr_address);
	u32 mbox1 = upper_32_bits(handoff->wpr_address);
	u32 control, mailbox;
	int ret;

	nvkm_falcon_wr32(&gsp->falcon, 0x040, lower_32_bits(handoff->libos_address));
	nvkm_falcon_wr32(&gsp->falcon, 0x044, upper_32_bits(handoff->libos_address));
	ret = nvkm_falcon_fw_boot(&gsp->booter.load, &gsp->subdev,
				  true, &mbox0, &mbox1, 0, 0);
	if (ret) {
		nvkm_error(&gsp->subdev, "r615: SEC2 Booter load failed (%d)\n", ret);
		return ret;
	}

	nvkm_falcon_wr32(&gsp->falcon, 0x080, handoff->app_version);
	/* Public 615 permits the CPU already waiting/suspended for host init. */
	control = nvkm_falcon_rd32(&gsp->falcon, 0x1388);
	mailbox = nvkm_falcon_rd32(&gsp->falcon, 0x040);
	if (control == ~0U || mailbox == ~0U)
		return -ENODEV;
	if (!(control & 0x80) && !(mailbox & 0x80000000)) {
		nvkm_error(&gsp->subdev, "r615: GSP RISC-V not active after Booter\n");
		return -EIO;
	}
	nvkm_info(&gsp->subdev, "r615: SEC2 Booter completed, GSP RISC-V active\n");
	return 0;
}

int
r615_gsp_bootstrap_pm(struct nvkm_gsp *gsp, u64 sr_metadata, u32 app_version)
{
	u32 mbox0 = lower_32_bits(sr_metadata), mbox1 = upper_32_bits(sr_metadata);
	u32 control, mailbox;
	int ret;

	if (!sr_metadata || sr_metadata % R615_BOOT_PAGE ||
	    sr_metadata > (1ULL << 49) - R615_BOOT_PAGE || !gsp->r615_stopped)
		return -EINVAL;
	/* From this point a failed Booter may have started DMA. No cold reset,
	 * LibOS mailbox rewrite, FRTS init, queue relink or GSP_INIT on resume.
	 */
	gsp->r615_stopped = false;
	gsp->r615_teardown_stage = 0;
	ret = nvkm_falcon_fw_boot(&gsp->booter.load, &gsp->subdev,
				  true, &mbox0, &mbox1, 0, 0);
	if (ret)
		return ret;
	nvkm_falcon_wr32(&gsp->falcon, 0x080, app_version);
	control = nvkm_falcon_rd32(&gsp->falcon, 0x1388);
	mailbox = nvkm_falcon_rd32(&gsp->falcon, 0x040);
	if (control == ~0U || mailbox == ~0U)
		return -ENODEV;
	return (control & 0x80) || (mailbox & 0x80000000) ? 0 : -EIO;
}

/* Stable, grep-able progress markers for the diagnostic runner. The last
 * "stage=<name> ok" line before a failure names the step that failed next.
 */
#define R615_STAGE(gsp, name) nvkm_info(&(gsp)->subdev, "r615: stage=%s ok\n", (name))

int
r615_gsp_init(struct nvkm_gsp *gsp)
{
	struct r615_boot_handoff handoff;
	struct r615_transport *transport;
	const char *stage = "arm";
	int ret;

	if (!r615_lifecycle_compatible(gsp) || !gsp->r615_locks_ready ||
	    !gsp->r615_fwsec_ready || !gsp->r615_host || gsp->r615_quarantined)
		return -EOPNOTSUPP;
	if (gsp->running)
		return -EALREADY;
	if (gsp->r615_pm)
		return r615_pm_resume(gsp);
	if (gsp->sr.meta.data)
		return -EOPNOTSUPP; /* resume path not validated yet */
	if (!gsp->r615_sysinfo || !gsp->r615_sysinfo_count)
		return -EOPNOTSUPP;
	if (!gsp->r615_payload) {
		/* A fresh cold boot after fully stopped POWEROFF, never an attempted
		 * recovery of a partially live or quarantined owner.
		 */
		if (!gsp->r615_stopped || gsp->r615_teardown_stage != 3)
			return -EIO;
		ret = r615_gsp_stage(gsp);
		if (ret)
			return ret;
	}

	ret = r615_boot_payload_arm(gsp->r615_payload, &handoff);
	if (ret)
		return ret;

	gsp->r615_stopped = false;
	gsp->r615_teardown_stage = 0;
	gsp->r615_suspend_sent = false;
	gsp->r615_suspended = false;
	nvkm_info(&gsp->subdev, "r615: state monitor DMA=%016llx size=%u bytes\n",
		  (unsigned long long)handoff.state_monitor_address,
		  handoff.state_monitor_bytes);
	stage = "booter";
	ret = r615_gsp_bootstrap(gsp, &handoff);
	if (ret)
		goto fail;
	R615_STAGE(gsp, stage);

	stage = "statusq";
	transport = r615_boot_payload_transport(gsp->r615_payload);
	if (!transport) {
		ret = -EIO;
		goto fail;
	}
	/* The GSP publishes its RX header after Booter hands off; poll like
	 * NVIDIA's GspStatusQueueInit, with periodic state snapshots so a
	 * stuck RM shows whether its INIT log is still growing (slow init)
	 * or frozen (wedged before queue setup).
	 */
	{
		unsigned long deadline =
			jiffies + msecs_to_jiffies(30000);
		unsigned long slice;
		int i;

		for (i = 0; i < 6; i++) {
			slice = jiffies + msecs_to_jiffies(5000);
			do {
				ret = r615_transport_link_rx(transport);
				if (ret != -EAGAIN)
					break;
				msleep(1);
			} while (time_before(jiffies, slice) &&
				 time_before(jiffies, deadline));
			if (ret != -EAGAIN)
				break;
			nvkm_info(&gsp->subdev,
				  "r615: still waiting for status queue (%ds)\n",
				  5 * (i + 1));
			r615_init_debug_dump(gsp);
			if (time_after_eq(jiffies, deadline))
				break;
		}
		if (ret == -EAGAIN) {
			nvkm_info(&gsp->subdev,
				  "r615: status queue link timed out\n");
			ret = -ETIMEDOUT;
		}
	}
	if (ret)
		goto fail;
	R615_STAGE(gsp, stage);
	stage = "rpc";
	ret = r615_rpc_new(transport, &gsp->r615_rpc);
	if (ret)
		goto fail;
	ret = r615_runtime_new(gsp);
	if (ret)
		goto fail_rpc;

	{
		u64 *static_words;
		u32 static_count = 0;
		struct r615_static *decoded;

		stage = "gsp_init";
		static_words = kvcalloc(R615_GMCAPI_MAX_STATIC_WORDS,
					  sizeof(u64), GFP_KERNEL);
		if (!static_words) {
			ret = -ENOMEM;
			goto fail_rpc;
		}
		ret = r615_rpc_gsp_init(gsp->r615_rpc,
					gsp->r615_sysinfo,
					gsp->r615_sysinfo_count,
					static_words,
					R615_GMCAPI_MAX_STATIC_WORDS,
					&static_count);
		if (ret) {
			kvfree(static_words);
			goto fail_rpc;
		}
		nvkm_info(&gsp->subdev, "r615: stage=gsp_init ok static_words=%u\n", static_count);
		stage = "static_decode";
		decoded = kzalloc_obj(*decoded, GFP_KERNEL);
		if (!decoded) {
			kvfree(static_words);
			ret = -ENOMEM;
			goto fail_rpc;
		}
		ret = r615_static_decode(static_words, static_count, decoded);
		if (ret) {
			kfree(decoded);
			kvfree(static_words);
			goto fail_rpc;
		}
		gsp->r615_static_words = static_words;
		gsp->r615_static_count = static_count;
		gsp->r615_static = decoded;
	}

	R615_STAGE(gsp, stage);
	gsp->running = true;

	stage = "static_info";
	ret = gsp->rm->api->gsp->get_static_info(gsp);
	if (WARN_ON(ret))
		goto fail_running;
	nvkm_info(&gsp->subdev, "r615: stage=static_info ok gpcs=%u tpcs=%u fb_regions=%d\n",
		  gsp->gr.gpcs, gsp->gr.tpcs, gsp->fb.region_nr);
	/* P-State: one-shot per GSP boot, with the internal subdevice handle that
	 * get_static_info() just published.  Diagnostic: a refusal or an unsupported
	 * control never fails the boot (RM keeps arbitrating the level).
	 */
	{
		int pret = nvkm_gsp_perf_max_performance(gsp);

		if (pret)
			nvkm_warn(&gsp->subdev, "r615: perf P0 request unavailable (%d)\n", pret);
	}
	stage = "health";
	ret = r615_health_start(gsp);
	if (ret)
		goto fail_running;
	ret = r615_health_recovery(gsp, 0, 0);
	if (!ret && READ_ONCE(gsp->r615_recovery_required))
		ret = -EIO;
	if (ret)
		goto fail_running;
	R615_STAGE(gsp, stage);
	stage = "intr";
	ret = r615_runtime_start(gsp);
	if (ret)
		goto fail_running;
	nvkm_info(&gsp->subdev, "r615: stage=intr ok vectors=%d\n", gsp->intr_nr);

	nvkm_info(&gsp->subdev, "r615: GSP_INIT ok, static decoded, running\n");
	nvkm_info(&gsp->subdev, "r615: stage=ready ok dropped_events=%llu\n",
		  (unsigned long long)r615_runtime_dropped(gsp));

	/* Telemetry is diagnostic: a refusal leaves sensors unavailable, it
	 * never fails the boot. Values are published only from a live page.
	 */
	{
		int tret = r615_telemetry_start(gsp);

		if (tret)
			nvkm_warn(&gsp->subdev, "r615: telemetry unavailable (%d)\n", tret);
		else
			nvkm_info(&gsp->subdev, "r615: telemetry RUSD active, poll %ums\n",
				  R615_TELEMETRY_INTERVAL_MS);
	}

	return 0;

fail_running:
	gsp->running = false;
	kvfree(gsp->r615_static_words);
	gsp->r615_static_words = NULL;
	gsp->r615_static_count = 0;
	kfree(gsp->r615_static);
	gsp->r615_static = NULL;
fail_rpc:
	r615_runtime_stop(gsp);
fail:
	nvkm_error(&gsp->subdev, "r615: init failed (%d) at stage=%s, stopping GSP\n", ret, stage);
	r615_init_debug_dump(gsp);
	/* Do not free exposed DMA if any hardware teardown stage failed. */
	{
		int stopped = r615_gsp_teardown(gsp);

		if (stopped) {
			nvkm_error(&gsp->subdev, "r615: teardown failed (%d), DMA retained\n", stopped);
			r615_gsp_save_logs(gsp); /* CPU PMU copy only: no proof DMA stopped. */
			r615_runtime_del(gsp);
			r615_rpc_del(&gsp->r615_rpc);
			return ret;
		}
		gsp->r615_stopped = true;
	}
	r615_gsp_save_logs(gsp);
	r615_runtime_del(gsp);
	r615_rpc_del(&gsp->r615_rpc);
	r615_health_del(gsp);
	if (r615_boot_payload_quiesce(gsp->r615_payload)) {
		nvkm_error(&gsp->subdev, "r615: quiesce failed, DMA retained\n");
		return ret;
	}
	if (r615_boot_payload_del(&gsp->r615_payload))
		nvkm_error(&gsp->subdev, "r615: payload still exposed, DMA retained\n");
	return ret;
}

int
r615_gsp_fini(struct nvkm_gsp *gsp, enum nvkm_suspend_state suspend)
{
	u32 mailbox;
	bool removed = false;
	int ret;

	if (!r615_lifecycle_compatible(gsp))
		return -EOPNOTSUPP;
	if (suspend == NVKM_SUSPEND && !gsp->sr.meta.data)
		return r615_pm_suspend(gsp);
	if (suspend != NVKM_POWEROFF || gsp->sr.meta.data)
		return -EOPNOTSUPP;
	if (gsp->r615_pm)
		return -EBUSY; /* Never reinterpret incomplete PM as a normal unload. */
	if (!gsp->running || !gsp->r615_payload || !gsp->r615_rpc)
		return -EOPNOTSUPP;

	/* Unmap the shared page while GSP still serves RPC. */
	if (r615_telemetry_stop(gsp))
		nvkm_warn(&gsp->subdev, "r615: telemetry unmap not acknowledged, page retained\n");
	r615_runtime_stop(gsp);
	if (!gsp->r615_suspend_sent) {
		/* Close the runtime API gate before sending the last command. */
		gsp->r615_suspend_sent = true;
		ret = r615_rpc_gsp_suspend(gsp->r615_rpc);
		if (ret)
			return ret;
	}
	if (!gsp->r615_suspended) {
		ret = nvkm_msec(gsp->subdev.device, 2000,
			mailbox = nvkm_falcon_rd32(&gsp->falcon, 0x040);
			if (mailbox == ~0U) { removed = true; break; }
			if (mailbox & 0x80000000)
				break;
		);
		if (removed)
			return -ENODEV;
		if (ret < 0)
			return -ETIMEDOUT;
		gsp->r615_suspended = true;
	}
	ret = r615_gsp_teardown(gsp);
	if (ret)
		return ret;
	ret = r615_health_del(gsp);
	if (ret)
		return ret;
	r615_telemetry_quiesced(gsp); /* DMA stop proven by teardown above */
	r615_telemetry_del(gsp);
	r615_gsp_save_logs(gsp);
	r615_runtime_del(gsp);
	r615_rpc_del(&gsp->r615_rpc);
	ret = r615_boot_payload_quiesce(gsp->r615_payload);
	if (ret)
		return ret;
	ret = r615_boot_payload_del(&gsp->r615_payload);
	if (ret)
		return ret;
	kvfree(gsp->r615_static_words);
	gsp->r615_static_words = NULL;
	gsp->r615_static_count = 0;
	kfree(gsp->r615_static);
	gsp->r615_static = NULL;
	return 0;
}

void
r615_gsp_dtor(struct nvkm_gsp *gsp)
{
	if (!gsp || gsp->r615_quarantined)
		return;
	if (gsp->running)
		r615_telemetry_stop(gsp);
	r615_runtime_stop(gsp);
	r615_gsp_save_logs(gsp);
	r615_runtime_del(gsp);
	r615_host_del(gsp);
	/* del refuses externally exposed memory without successful quiescence.
	 * Retain WPR metadata too when reset/unload failed; freeing only part
	 * of an exposed boot payload would still allow DMA into freed memory.
	 */
	if (gsp->r615_health || gsp->r615_pm || gsp->running || r615_boot_payload_del(&gsp->r615_payload)) {
		/* A failed normal fini must not strand a live GSP at device removal. */
		if (r615_gsp_teardown(gsp) || r615_pm_del(gsp) || r615_health_del(gsp) ||
		    r615_boot_payload_quiesce(gsp->r615_payload) ||
		    r615_boot_payload_del(&gsp->r615_payload)) {
			r615_rpc_del(&gsp->r615_rpc);
			r615_boot_payload_quarantine(gsp->r615_payload);
			mutex_lock(&r615_quarantine_lock);
			if (!gsp->r615_quarantined) {
				gsp->r615_quarantined = true;
				list_add_tail(&gsp->r615_quarantine_entry, &r615_quarantine);
			}
			mutex_unlock(&r615_quarantine_lock);
			nvkm_error(&gsp->subdev, "r615: owner/DMA quarantined until reboot\n");
			return;
		}
	}
	r615_rpc_del(&gsp->r615_rpc);
	r615_telemetry_quiesced(gsp); /* only reached after a proven stop */
	r615_telemetry_del(gsp);
	kvfree(gsp->r615_static_words);
	kfree(gsp->r615_static);
	kvfree(gsp->r615_sysinfo);
	gsp->r615_sysinfo = NULL;
	nvkm_gsp_mem_dtor(&gsp->wpr_meta);
	if (gsp->r615_locks_ready) {
		idr_destroy(&gsp->client_id.idr);
		mutex_destroy(&gsp->client_id.mutex);
		mutex_destroy(&gsp->msgq.mutex);
		mutex_destroy(&gsp->cmdq.mutex);
		gsp->r615_locks_ready = false;
	}
	nvkm_falcon_fw_dtor(&gsp->booter.unload);
	nvkm_falcon_fw_dtor(&gsp->booter.load);
	if (gsp->r615_fwsec_ready) {
		nvkm_gsp_fwsec_sb_dtor(gsp);
		gsp->r615_fwsec_ready = false;
	}
	nvkm_gsp_dtor_fws(gsp);
}
