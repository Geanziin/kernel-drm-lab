// SPDX-License-Identifier: MIT
/* Native GA107 non-replayable faults, FECS tracing-error containment and
 * recovery-action policy. All fallible controls run outside the RPC RX lock.
 */
#include <linux/dma-mapping.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <core/device.h>
#include <core/gpuobj.h>
#include <engine/fifo/chan.h>
#include <rm/rm.h>
#include "dma_image.h"
#include "health.h"

/* Public 615 registers the client shadow NR buffer only for the MMU_FAULT_BUFFER
 * class owned by UVM; ordinary channel faults still arrive as RC_TRIGGERED.
 * Nouveau has no UVM consumer, so registration is an explicit opt-in that
 * changes firmware behaviour and is NOT required for cold boot.
 */
static bool r615_mmu_shadow;
module_param(r615_mmu_shadow, bool, 0400);
MODULE_PARM_DESC(r615_mmu_shadow, "r615: register NR MMU shadow fault queue (experimental, default off)");

struct r615_health {
	struct device *device;
	struct mutex mutex;
	struct r615_dma_image *image;
	struct r615_fault_queue *queue;
	dma_addr_t queue_address;
	struct r615_fault_plan plan;
	struct r615_recovery_state recovery;
	u64 get, faults, stale, fecs_errors;
	bool exposed, ready, failed, tracing_disabled;
};

static int
r615_health_control(struct nvkm_gsp *gsp, u32 command, void *data, u32 bytes, bool read)
{
	struct nvkm_gsp_object *object = &gsp->internal.device.subdevice;
	void *request = nvkm_gsp_rm_ctrl_get(object, command, bytes);
	int ret;

	if (IS_ERR(request))
		return PTR_ERR(request);
	memcpy(request, data, bytes);
	ret = nvkm_gsp_rm_ctrl_push(object, &request, read ? bytes : 0);
	if (ret) {
		nvkm_gsp_rm_ctrl_done(object, request); /* NULL-safe */
		nvkm_error(&gsp->subdev, "r615: health control %08x failed (%d)\n", command, ret);
		return ret;
	}
	if (read) {
		memcpy(data, request, bytes);
		nvkm_gsp_rm_ctrl_done(object, request);
	}
	return 0;
}

static int
r615_health_contain(struct nvkm_gsp *gsp)
{
	u8 disable = 1;
	int ret;

	WRITE_ONCE(gsp->r615_recovery_required, true);
	if (gsp->r615_health->recovery.contained)
		return 0;
	ret = r615_health_control(gsp, R615_DISABLE_USER_CHANNELS, &disable, sizeof(disable), false);
	if (!ret)
		gsp->r615_health->recovery.contained = true;
	return ret;
}

int
r615_health_recovery(struct nvkm_gsp *gsp, u32 type, u8 value)
{
	struct r615_health *health = gsp->r615_health;
	int ret;

	if (!health || !health->ready)
		return -ESHUTDOWN;
	mutex_lock(&health->mutex);
	if (health->failed) {
		mutex_unlock(&health->mutex);
		return -EIO;
	}
	ret = r615_recovery_update(&health->recovery, type, value,
				   nvkm_rd32(gsp->subdev.device, R615_RESET_SCRATCH));
	if (!ret && health->recovery.action)
		ret = r615_health_contain(gsp);
	if (!ret && (health->recovery.action || health->recovery.requests))
		nvkm_warn(&gsp->subdev, "r615: recovery action=%u requests=%x reset=%d drain=%d contained=%d\n",
			  health->recovery.action, health->recovery.requests, health->recovery.reset,
			  health->recovery.drain, health->recovery.contained);
	else if (!ret)
		nvkm_info(&gsp->subdev, "r615: recovery action=none reset=%d drain=%d\n",
			  health->recovery.reset, health->recovery.drain);
	/* A cleared request does not resume channels, unpoison RPC, or discard
	 * retained DMA. Re-enabling needs a proven recovery of all old contexts.
	 */
	if (ret) {
		health->failed = true;
		WRITE_ONCE(gsp->r615_recovery_required, true);
	}
	mutex_unlock(&health->mutex);
	return ret;
}

int
r615_health_fecs(struct nvkm_gsp *gsp, const struct r615_fecs_event *event)
{
	struct r615_health *health = gsp->r615_health;
	struct r615_fecs_enable params = { 0 };
	int ret;

	if (!health || !health->ready || !event || event->gr || event->type < 1 || event->type > 2)
		return -EINVAL;
	mutex_lock(&health->mutex);
	if (health->failed) {
		mutex_unlock(&health->mutex);
		return -EIO;
	}
	/* Bare-metal GA107 has one GR; public routing is all-zero outside MIG.
	 * Nouveau has no FECS tracing subscriber/buffer binding. Keep hardware
	 * logging disabled instead of pretending to drain an unowned trace ring.
	 * This handles tracing errors, NOT a reset of the GR execution engine.
	 */
	ret = r615_health_control(gsp, R615_FECS_SET_ENABLE, &params, sizeof(params), false);
	if (ret)
		goto done;
	ret = r615_health_control(gsp, R615_FECS_GET_ENABLE, &params, sizeof(params), true);
	if (!ret && (params.flags || params.route || params.enable))
		ret = -EIO;
	if (!ret) {
		health->tracing_disabled = true;
		health->fecs_errors++;
		nvkm_warn(&gsp->subdev, "r615: FECS tracing error=%u contained, no tracing consumer enabled\n", event->type);
	}
done:
	if (ret) {
		health->failed = true;
		WRITE_ONCE(gsp->r615_recovery_required, true);
	}
	mutex_unlock(&health->mutex);
	return ret;
}

static int
r615_health_fault_channel(struct nvkm_gsp *gsp, const struct r615_fault_record *fault)
{
	struct nvkm_fifo *fifo = gsp->subdev.device->fifo;
	struct nvkm_chan *channel;
	struct nvkm_gpuobj *object;
	unsigned long flags;
	u32 aperture = ~0U;
	int ret = 0;

	nvkm_error(&gsp->subdev, "r615: MMU fault inst=%llx aperture=%u addr=%llx engine=%u type=%u client=%u access=%u\n",
		   fault->instance, fault->aperture, fault->address, fault->engine, fault->type,
		   fault->client, fault->access);
	if (!fifo)
		return -ENODEV;
	channel = nvkm_chan_get_inst(&fifo->engine, fault->instance, &flags);
	if (!channel) {
		gsp->r615_health->stale++;
		return 0; /* A fault for a removed context is still consumed. */
	}
	object = channel->inst;
	while (object && !object->memory)
		object = object->parent;
	if (object) {
		switch (nvkm_memory_target(object->memory)) {
		case NVKM_MEM_TARGET_VRAM: aperture = 0; break;
		case NVKM_MEM_TARGET_HOST: aperture = 2; break;
		case NVKM_MEM_TARGET_NCOH: aperture = 3; break;
		default: break;
		}
	}
	if (aperture != fault->aperture)
		ret = -EPROTO;
	else
		nvkm_chan_error(channel, false); /* notify nouveau's userspace fence/error path */
	nvkm_chan_put(&channel, flags);
	return ret;
}

int
r615_health_faults(struct nvkm_gsp *gsp, u32 budget)
{
	struct r615_health *health = gsp->r615_health;
	struct r615_fault_queue *queue;
	u32 count = 0;
	int ret = 0;

	if (!health || !health->ready || !budget || budget > 256)
		return -EINVAL;
	mutex_lock(&health->mutex);
	if (health->failed) {
		mutex_unlock(&health->mutex);
		return -EIO;
	}
	queue = health->queue;
	if (!queue) {
		mutex_unlock(&health->mutex);
		return -ENODEV; /* no shadow registered: not a consumer failure */
	}
	while (count < budget) {
		struct r615_fault_record fault;
		u32 packet[8];
		u64 put = READ_ONCE(queue->put);

		if (READ_ONCE(queue->capacity) != health->plan.capacity ||
		    READ_ONCE(queue->message_bytes) != R615_FAULT_BYTES ||
		    READ_ONCE(queue->allocator) || READ_ONCE(queue->data) ||
		    READ_ONCE(queue->get) != health->get || put >= health->plan.capacity) {
			ret = -EPROTO;
			break;
		}
		if (health->get == put)
			break;
		dma_rmb();
		ret = r615_dma_image_copy(health->image, health->get * R615_FAULT_BYTES, packet, sizeof(packet));
		if (!ret)
			ret = r615_fault_decode(packet, &fault);
		if (ret)
			break;
		/* Non-replayable errors cannot be retried by a missing UVM consumer.
		 * Actually disable user scheduling before acknowledging these packets.
		 * Keep internal RM/health traffic alive for diagnostics and cleanup.
		 */
		ret = r615_health_contain(gsp);
		if (!ret)
			ret = r615_health_fault_channel(gsp, &fault);
		if (ret)
			break;
		health->get = (health->get + 1) % health->plan.capacity;
		dma_wmb();
		WRITE_ONCE(queue->get, health->get);
		health->faults++;
		count++;
	}
	if (ret) {
		health->failed = true;
		WRITE_ONCE(gsp->r615_recovery_required, true);
	}
	mutex_unlock(&health->mutex);
	return ret ? ret : (int)count;
}

int
r615_health_start(struct nvkm_gsp *gsp)
{
	struct r615_gmmu_static info = { 0 };
	struct r615_shadow_register *registration;
	struct r615_health *health;
	u64 root, bytes;
	int ret;

	if (!gsp || !gsp->subdev.device || gsp->r615_health || !gsp->rm ||
	    !gsp->rm->version || strcmp(gsp->rm->version, "615.78.08") ||
	    gsp->subdev.device->chipset != 0x177)
		return -EOPNOTSUPP;
	health = kzalloc_obj(*health, GFP_KERNEL);
	if (!health)
		return -ENOMEM;
	health->device = get_device(gsp->subdev.device->dev);
	mutex_init(&health->mutex);
	gsp->r615_health = health;
	if (!r615_mmu_shadow) {
		/* REFRESH/FECS containment stay active; no DMA is exposed. */
		health->ready = true;
		nvkm_info(&gsp->subdev, "r615: MMU shadow queue not registered (opt-in r615_mmu_shadow=1)\n");
		return 0;
	}
	ret = r615_health_control(gsp, R615_GMMU_STATIC, &info, sizeof(info), true);
	if (ret)
		return ret;
	ret = r615_fault_plan(&info, &health->plan);
	if (ret)
		return ret;
	ret = r615_dma_image_zero_new(health->device, health->plan.total_bytes, &health->image);
	if (ret)
		return ret;
	health->queue = dma_alloc_coherent(health->device, 4096, &health->queue_address, GFP_KERNEL);
	if (!health->queue)
		return -ENOMEM;
	memset(health->queue, 0, 4096);
	if (!health->queue_address || health->queue_address % 4096 || health->queue_address > (1ULL << 49) - 4096)
		return -EINVAL;
	health->queue->capacity = health->plan.capacity;
	health->queue->message_bytes = R615_FAULT_BYTES;
	registration = kvzalloc(sizeof(*registration), GFP_KERNEL);
	if (!registration)
		return -ENOMEM;
	registration->queue = health->queue_address;
	registration->bytes = health->plan.bytes;
	registration->metadata_bytes = health->plan.metadata_bytes;
	ret = r615_dma_image_pages(health->image, registration->pages, health->plan.pages);
	if (ret)
		goto done;
	ret = r615_dma_image_expose(health->image, &root, &bytes);
	if (ret)
		goto done;
	dma_wmb();
	health->exposed = true; /* retain even after an ambiguous registration error */
	ret = r615_health_control(gsp, R615_GMMU_REGISTER_SHADOW, registration, sizeof(*registration), false);
	if (!ret) {
		health->ready = true;
		nvkm_info(&gsp->subdev, "r615: MMU shadow registered pages=%u entries=%u queue=%llx\n",
			  health->plan.pages, health->plan.capacity, (u64)health->queue_address);
	}
done:
	kvfree(registration);
	return ret;
}

int
r615_health_del(struct nvkm_gsp *gsp)
{
	struct r615_health *health = gsp->r615_health;

	if (!health)
		return 0;
	/* The GSP is the sole producer. A failed registration/unregister is NOT
	 * proof of stop. Runtime work must already be stopped by the lifecycle.
	 */
	if (health->exposed && !gsp->r615_stopped)
		return -EBUSY;
	r615_dma_image_quiesced(health->image);
	r615_dma_image_del(&health->image);
	if (health->queue)
		dma_free_coherent(health->device, 4096, health->queue, health->queue_address);
	put_device(health->device);
	mutex_destroy(&health->mutex);
	kfree(health);
	gsp->r615_health = NULL;
	return 0;
}
