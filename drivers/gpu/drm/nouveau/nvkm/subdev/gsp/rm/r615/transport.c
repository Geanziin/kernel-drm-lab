// SPDX-License-Identifier: MIT
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <core/device.h>
#include <rm/rm.h>

#include "transport.h"

struct r615_transport {
	struct nvkm_gsp *gsp;
	struct device *dma_device;
	/* Serializes queue counters, staging and exposure/quiescence state. */
	struct mutex mutex;
	void *memory;
	dma_addr_t address;
	void *tx_frame;
	struct r615_msgq queue;
	struct r615_init_args args;
	r615_transport_quiesce_func quiesce;
	void *context;
	bool exposed, stopped, quarantined, quiescing;
	bool pm_paused, pm_verified;
};

/* Public 615 TU102 register HAL also selected for GA107 in g_gpu_nvoc.c. */
static const u32 r615_transport_registers[] = {
	[R615_MSGQ_TX_HEAD] = 0x110c00,
	[R615_MSGQ_TX_TAIL] = 0x110c04,
	[R615_MSGQ_RX_HEAD] = 0x110c80,
	[R615_MSGQ_RX_TAIL] = 0x110c84,
};

static bool
r615_transport_compatible(struct nvkm_gsp *gsp)
{
	return gsp && gsp->subdev.device && gsp->subdev.device->dev &&
	       gsp->subdev.device->chipset == 0x177 && gsp->rm && gsp->rm->version &&
	       !strcmp(gsp->rm->version, "615.78.08");
}

static int
r615_transport_read(void *context, enum r615_msgq_register reg, u32 *value)
{
	struct r615_transport *transport = context;

	if (reg != R615_MSGQ_TX_TAIL && reg != R615_MSGQ_RX_HEAD)
		return -EINVAL;
	/* All u32 counter values, including ~0U, are legal at wrap. */
	*value = nvkm_rd32(transport->gsp->subdev.device, r615_transport_registers[reg]);
	return 0;
}

static int
r615_transport_write(void *context, enum r615_msgq_register reg, u32 value)
{
	struct r615_transport *transport = context;

	if (reg != R615_MSGQ_TX_HEAD && reg != R615_MSGQ_RX_TAIL)
		return -EINVAL;
	/* nvkm_wr32 has no completion/error result; no invented MMIO acknowledgment. */
	nvkm_wr32(transport->gsp->subdev.device, r615_transport_registers[reg], value);
	return 0;
}

static void
r615_transport_acquire(void *context)
{
	dma_rmb();
}

static void
r615_transport_publish(void *context)
{
	/* Order RX reads/SEEN stores as well as TX writes before counter publication. */
	dma_mb();
}

static const struct r615_msgq_ops r615_transport_ops = {
	.read = r615_transport_read,
	.write = r615_transport_write,
	.acquire = r615_transport_acquire,
	.publish = r615_transport_publish,
};

int
r615_transport_new(struct nvkm_gsp *gsp, r615_transport_quiesce_func quiesce,
		   void *context, struct r615_transport **output)
{
	struct r615_transport *transport;
	int ret;

	if (!output || *output || !quiesce)
		return -EINVAL;
	if (!r615_transport_compatible(gsp))
		return -EOPNOTSUPP;
	if (gsp->running)
		return -EBUSY;
	transport = kzalloc_obj(*transport, GFP_KERNEL);
	if (!transport)
		return -ENOMEM;
	transport->gsp = gsp;
	transport->dma_device = gsp->subdev.device->dev;
	transport->quiesce = quiesce;
	transport->context = context;
	mutex_init(&transport->mutex);
	transport->memory = dma_alloc_coherent(transport->dma_device, R615_MSGQ_SHARED_BYTES,
					       &transport->address, GFP_KERNEL);
	if (!transport->memory) {
		ret = -ENOMEM;
		goto fail;
	}
	transport->tx_frame = kvzalloc(R615_MESSAGE_MAX_BYTES, GFP_KERNEL);
	if (!transport->tx_frame) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = r615_msgq_prepare(&transport->queue, gsp->rm->version, transport->memory,
				R615_MSGQ_SHARED_BYTES, transport->address, &r615_transport_ops,
				transport, &transport->args);
	if (ret)
		goto fail;
	*output = transport;
	return 0;
fail:
	r615_transport_del(&transport);
	return ret;
}

static int
r615_transport_ready(struct r615_transport *transport)
{
	if (transport->stopped)
		return -ESHUTDOWN;
	if (transport->quarantined || transport->queue.failed)
		return -EIO;
	if (transport->pm_paused)
		return -ESHUTDOWN;
	if (!r615_transport_compatible(transport->gsp))
		return -EOPNOTSUPP;
	if (!transport->exposed)
		return -EAGAIN;
	return 0;
}

int
r615_transport_arm(struct r615_transport *transport, struct r615_init_args *output)
{
	int ret;

	if (!transport || !output)
		return -EINVAL;
	mutex_lock(&transport->mutex);
	if (transport->stopped) {
		ret = -ESHUTDOWN;
	} else if (transport->quarantined) {
		ret = -EIO;
	} else if (transport->exposed) {
		ret = -EALREADY;
	} else if (!r615_transport_compatible(transport->gsp)) {
		ret = -EOPNOTSUPP;
	} else if (transport->gsp->running) {
		ret = -EBUSY;
	} else {
		/* Treat even an uncertain first MMIO write as externally exposed. */
		transport->exposed = true;
		ret = r615_msgq_start(&transport->queue);
		if (!ret)
			*output = transport->args;
	}
	mutex_unlock(&transport->mutex);
	return ret;
}

int
r615_transport_link_rx(struct r615_transport *transport)
{
	int ret;

	if (!transport)
		return -EINVAL;
	mutex_lock(&transport->mutex);
	ret = r615_transport_ready(transport);
	if (!ret)
		ret = r615_msgq_link_rx(&transport->queue);
	mutex_unlock(&transport->mutex);
	return ret;
}

int
r615_transport_send(struct r615_transport *transport, const struct r615_message *message)
{
	int ret;

	if (!transport)
		return -EINVAL;
	mutex_lock(&transport->mutex);
	ret = r615_transport_ready(transport);
	if (!ret)
		ret = r615_message_send(&transport->queue, message, transport->tx_frame,
					R615_MESSAGE_MAX_BYTES);
	mutex_unlock(&transport->mutex);
	return ret;
}

int
r615_transport_receive(struct r615_transport *transport, void *frame, size_t capacity,
		       struct r615_message *output)
{
	int ret;

	if (!transport)
		return -EINVAL;
	mutex_lock(&transport->mutex);
	ret = r615_transport_ready(transport);
	if (!ret)
		ret = r615_message_receive(&transport->queue, frame, capacity, output);
	mutex_unlock(&transport->mutex);
	return ret;
}

int
r615_transport_pm_pause(struct r615_transport *transport)
{
	int ret;

	if (!transport)
		return -EINVAL;
	mutex_lock(&transport->mutex);
	if (transport->stopped || transport->quarantined) {
		ret = -ESHUTDOWN;
		goto done;
	}
	if (transport->quiescing) {
		ret = -EBUSY;
		goto done;
	}
	if (!transport->exposed || !transport->queue.rx_linked || transport->queue.failed ||
	    !r615_transport_compatible(transport->gsp)) {
		ret = -EINVAL;
		goto done;
	}
	if (transport->pm_verified) {
		ret = -EALREADY;
		goto done;
	}
	/* Block MMIO even if the owner's halt proof fails. A failed hold may be
	 * retried, but cannot authorize args writes, resume, or freeing memory.
	 */
	transport->pm_paused = true;
	transport->quiescing = true;
	mutex_unlock(&transport->mutex);
	ret = transport->quiesce ? transport->quiesce(transport->gsp, transport->context) : -ESHUTDOWN;
	mutex_lock(&transport->mutex);
	transport->quiescing = false;
	if (!ret && !transport->stopped && !transport->quarantined)
		transport->pm_verified = true;
	else if (!ret)
		ret = -ESHUTDOWN;
done:
	mutex_unlock(&transport->mutex);
	return ret;
}

static int
r615_transport_pm_ready(struct r615_transport *transport)
{
	if (transport->stopped || transport->quarantined)
		return -ESHUTDOWN;
	if (!transport->pm_paused || !transport->pm_verified || transport->quiescing)
		return -EBUSY;
	if (transport->queue.failed || !r615_transport_compatible(transport->gsp))
		return -EIO;
	if (transport->gsp->running)
		return -EBUSY;
	return 0;
}

int
r615_transport_pm_args(struct r615_transport *transport, struct r615_init_args *output)
{
	int ret;

	if (!transport || !output)
		return -EINVAL;
	mutex_lock(&transport->mutex);
	ret = r615_transport_pm_ready(transport);
	if (!ret)
		*output = transport->args;
	mutex_unlock(&transport->mutex);
	return ret;
}

int
r615_transport_pm_resume(struct r615_transport *transport)
{
	int ret;

	if (!transport)
		return -EINVAL;
	mutex_lock(&transport->mutex);
	ret = r615_transport_pm_ready(transport);
	if (!ret) {
		transport->pm_paused = false;
		transport->pm_verified = false;
	}
	mutex_unlock(&transport->mutex);
	return ret;
}

int
r615_transport_quiesce(struct r615_transport *transport)
{
	int ret = 0;

	if (!transport)
		return -EINVAL;
	mutex_lock(&transport->mutex);
	if (transport->stopped)
		goto done;
	if (transport->quiescing) {
		ret = -EBUSY;
		goto done;
	}
	transport->quarantined = true;
	if (!transport->exposed) {
		transport->stopped = true;
		goto done;
	}
	transport->quiescing = true;
	/* A worker being drained by the boot owner may itself need this mutex. */
	mutex_unlock(&transport->mutex);
	ret = transport->quiesce ? transport->quiesce(transport->gsp, transport->context) :
		-ESHUTDOWN;
	mutex_lock(&transport->mutex);
	transport->quiescing = false;
	if (!ret)
		transport->stopped = true;
done:
	mutex_unlock(&transport->mutex);
	return ret;
}

/* Irreversible until reboot. No callback/device-owner access after removal. */
void
r615_transport_quarantine(struct r615_transport *transport)
{
	if (!transport)
		return;
	mutex_lock(&transport->mutex);
	transport->quarantined = true;
	transport->gsp = NULL;
	transport->quiesce = NULL;
	transport->context = NULL;
	mutex_unlock(&transport->mutex);
}

int
r615_transport_del(struct r615_transport **output)
{
	struct r615_transport *transport;

	if (!output || !*output)
		return 0;
	transport = *output;
	/* Boot owner serializes deletion with every caller, including IRQ/work. */
	mutex_lock(&transport->mutex);
	if (transport->exposed && !transport->stopped) {
		mutex_unlock(&transport->mutex);
		return -EBUSY;
	}
	if (transport->memory)
		dma_free_coherent(transport->dma_device, R615_MSGQ_SHARED_BYTES,
				  transport->memory, transport->address);
	kvfree(transport->tx_frame);
	mutex_unlock(&transport->mutex);
	mutex_destroy(&transport->mutex);
	kfree(transport);
	*output = NULL;
	return 0;
}
