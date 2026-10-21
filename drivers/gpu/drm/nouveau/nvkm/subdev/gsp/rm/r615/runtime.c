// SPDX-License-Identifier: MIT
/* Native 615 queue IRQ, deferred notification delivery and interrupt map. */
#include <linux/interrupt.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <core/device.h>
#include <core/gpuobj.h>
#include <engine/fifo/cgrp.h>
#include <engine/fifo/chan.h>
#include <engine/fifo/runl.h>
#include <subdev/vfn.h>
#include <rm/rm.h>
#include "message.h"
#include "rpc.h"
#include "runtime.h"
#include "host_exec.h"
#include "libos.h"
#include "health.h"

#define R615_EVENT_QUEUE_LIMIT 128U
#define R615_EVENT_WORK_BUDGET 8U
#define R615_FAULT_WORK_BUDGET 64U
static int r615_runtime_queue(void *, const struct r615_message *);
struct r615_pending_event {
	struct list_head head;
	u32 function, bytes;
	u8 data[];
};
struct r615_runtime {
	struct nvkm_gsp *gsp;
	struct work_struct rx_work, event_work;
	struct nvkm_inth inth;
	spinlock_t lock;
	struct list_head events;
	u32 queued;
	struct r615_libos_capture libos;
	/* Public-enum events with no nouveau consumer: counted, first one logged. */
	u64 dropped, dropped_seen;
	bool stopped, registered, started, lockdown, pm_paused, mmu_queued;
	/* RC (robust channel) diagnostics.  The policy decides whether an event
	 * only updates these counters or also tears the affected group down.
	 */
	u64 rc_events, rc_logged, rc_actions;
	u32 rc_policy_seen;
};

/* Public 615 g_kernel_rc_nvoc.h: RC_NOTIFIER_SCOPE_CHANNEL/TSG.  A TSG-scoped
 * notification is about the whole channel group, not the single channel named
 * by chid, so the host cannot disable one arbitrary channel for it.
 */
#define R615_RC_SCOPE_CHANNEL 0U
#define R615_RC_SCOPE_TSG     1U
/* Full-detail RC logs before the summary-only rate limit kicks in. */
#define R615_RC_LOG_BUDGET    8U

/* Host action on R615_EVENT_RC:
 *   0 = diagnose only.  GSP-RM contains the fault before notifying the host,
 *       but a halted channel never signals its pending fences, so userspace
 *       waits forever on them.
 *   1 = upstream nouveau parity: disable the single channel named by chid.
 *   2 = channel-group recovery (default): run nouveau's runlist/TSG RC
 *       recovery (block runlist, disable group channels, reset the engine
 *       that was running the group).  Needed for TSG-scoped GR errors, where
 *       disabling one channel leaves the rest of the group wedged.
 */
static uint r615_rc_policy = 2;
module_param_named(rc_policy, r615_rc_policy, uint, 0644);
MODULE_PARM_DESC(rc_policy,
		 "GSP-RM robust-channel host action: 0=diagnose only, 1=disable channel by chid, 2=channel-group recovery (default 2)");

/* Bytes of the RM fault journal to hex-dump on the first RC event of the boot.
 * The journal is where RM records what the engine was doing (9816 bytes in the
 * boot that took Xwayland's channel down), so it is the primary evidence when
 * chasing the producer of a GR exception.
 */
static uint r615_rc_journal_bytes = 16384;
module_param_named(rc_journal_bytes, r615_rc_journal_bytes, uint, 0644);
MODULE_PARM_DESC(rc_journal_bytes,
		 "GSP-RM fault journal bytes to dump on the first robust-channel event (default 16384, max 16384)");

/* Public 615 rpc_global_enums.h: FIRST_EVENT 0x1000 < event < NUM_EVENTS 0x102e. */
#define R615_EVENT_PUBLIC_FIRST 0x1000U
#define R615_EVENT_PUBLIC_END 0x102eU

static int
r615_runtime_post(struct nvkm_gsp *gsp, const void *data, u32 bytes)
{
	const struct r615_post_event *message = data;
	struct nvkm_gsp_client *client;
	struct nvkm_gsp_event *event, *source = NULL;
	int ret = r615_post_event_validate(data, bytes);

	if (ret)
		return ret;
	ret = -ENOENT;
	mutex_lock(&gsp->client_id.mutex);
	client = idr_find(&gsp->client_id.idr, message->client & 0xffff);
	if (client && client->object.handle == message->client) {
		list_for_each_entry(event, &client->events, head) {
			if (event->object.handle == message->event) {
				source = event;
				break;
			}
		}
		/* Public _kgspRpcPostEvent: bNotifyList targets matching notifier
		 * indices on the source notifier, not merely the named hEvent.
		 */
		list_for_each_entry(event, &client->events, head) {
			if (source && (message->notify_list ?
			    event->device == source->device && event->id == message->index :
			    event == source)) {
				event->func(event, (void *)message->payload, message->bytes);
				ret = 0;
			}
		}
	}
	mutex_unlock(&gsp->client_id.mutex);
	return ret;
}

/* The RC notification names an RM engine type.  The channel group of the
 * reported channel must own a context for that engine, otherwise the event is
 * about a different channel and this one must not be touched.
 */
static bool
r615_rc_cgrp_has_engine(struct nvkm_gsp *gsp, struct nvkm_cgrp *cgrp, u32 engine,
			const char **name)
{
	const struct nvkm_rm_api_fifo *api = gsp->rm ? gsp->rm->api->fifo : NULL;
	enum nvkm_subdev_type type;
	struct nvkm_ectx *ectx;
	int inst, p2080;

	if (name)
		*name = NULL;
	if (!api || !api->xlat_rm_engine_type)
		return false;
	inst = api->xlat_rm_engine_type(engine, &type, &p2080);
	if (inst < 0)
		return false;

	list_for_each_entry(ectx, &cgrp->ectxs, head) {
		if (ectx->engn->engine->subdev.type != type ||
		    ectx->engn->engine->subdev.inst != inst)
			continue;
		if (name)
			*name = ectx->engn->engine->subdev.name;
		return true;
	}
	return false;
}

static int
r615_runtime_rc(struct nvkm_gsp *gsp, const struct r615_rc_event *message)
{
	struct r615_runtime *runtime = gsp->r615_runtime;
	struct nvkm_fifo *fifo = gsp->subdev.device->fifo;
	struct nvkm_chan *channel = NULL;
	struct nvkm_cgrp *cgrp = NULL;
	const char *engine_name = NULL;
	unsigned long flags = 0;
	bool attributed = false;
	u32 policy = READ_ONCE(r615_rc_policy);
	u64 event = 0;
	int ret = 0;

	if (!fifo)
		return -ENODEV;

	/* The channel is the only host object the event can map to: chid form, or
	 * instance form checked against the aperture of the instance block.
	 */
	if (!message->info_type) {
		channel = nvkm_chan_get_chid(&fifo->engine, message->chid, &flags);
	} else {
		u64 instance = (u64)message->inst_hi << 32 | message->inst_lo;

		channel = nvkm_chan_get_inst(&fifo->engine, instance, &flags);
		if (channel) {
			struct nvkm_gpuobj *object = channel->inst;
			u32 aperture = ~0U;

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
			if (aperture != message->aperture || aperture == ~0U)
				ret = -EPROTO;
		}
	}

	if (channel)
		cgrp = nvkm_cgrp_ref(channel->cgrp);
	if (cgrp)
		attributed = r615_rc_cgrp_has_engine(gsp, cgrp, message->engine,
						    &engine_name);

	if (runtime)
		event = ++runtime->rc_events;

	/* Rate limit: full detail for the first events, then a periodic summary,
	 * so a looping fault cannot flood the console while the counters keep the
	 * record.  The journal carries RM's own trace of the faulting context.
	 */
	if (runtime && runtime->rc_logged < R615_RC_LOG_BUDGET) {
		runtime->rc_logged++;
		nvkm_error(&gsp->subdev,
			   "r615: RC engn=%08x(%s) chid=%u cgrp=%s scope=%u level=%u type=%u gfid=%u part=%u fault=%08x%08x/%08x ap=%u cb=%u journal=%u policy=%u matched=%u\n",
			   message->engine, engine_name ? engine_name : "?",
			   message->chid, cgrp ? cgrp->name : "-",
			   message->scope, message->level, message->type, message->gfid,
			   message->partition, message->fault_hi, message->fault_lo,
			   message->fault_type, message->aperture, message->callback_needed,
			   message->journal_bytes, policy, attributed);
		if (message->journal_bytes && runtime->rc_logged <= R615_RC_LOG_BUDGET)
			print_hex_dump(KERN_INFO, "nvgd r615: rc journal ", DUMP_PREFIX_OFFSET,
				       16, 1, message->journal,
				       min_t(u32, message->journal_bytes,
					     min_t(u32, READ_ONCE(r615_rc_journal_bytes), 16384)),
				       false);
	} else if (runtime && !(event % 256)) {
		nvkm_warn(&gsp->subdev,
			  "r615: RC events=%llu (host actions=%llu) chid=%u type=%u scope=%u\n",
			  event, runtime->rc_actions, message->chid, message->type,
			  message->scope);
	}

	if (ret) {
		/* Instance/aperture disagreement: never act on a channel the event
		 * may not be about.
		 */
		goto out;
	}
	if (!policy || !message->callback_needed || !channel || !cgrp)
		goto out;
	if (!attributed) {
		nvkm_warn(&gsp->subdev,
			  "r615: RC chid=%u not attributed to engn=%08x, host stays out\n",
			  message->chid, message->engine);
		goto out;
	}

	if (policy == 1 || message->scope == R615_RC_SCOPE_CHANNEL) {
		nvkm_chan_error(channel, false);
	} else {
		/* TSG scope: the whole channel group is the recovery unit. */
		nvkm_runl_rc_cgrp(cgrp);
	}
	if (runtime)
		runtime->rc_actions++;

out:
	nvkm_cgrp_unref(&cgrp);
	nvkm_chan_put(&channel, flags);
	return ret;
}

static int
r615_runtime_event(struct nvkm_gsp *gsp, struct r615_pending_event *event)
{
	/* Engine-specific registrations still use nouveau's notification table.
	 * This is callback integration, not legacy queue framing.
	 */
	mutex_lock(&gsp->msgq.mutex);
	for (int i = 0; i < gsp->msgq.ntfy_nr; i++) {
		struct nvkm_gsp_msgq_ntfy notify = gsp->msgq.ntfy[i];

		if (!r615_event_native(event->function) &&
		    notify.fn == event->function && notify.func) {
			mutex_unlock(&gsp->msgq.mutex);
			return notify.func(notify.priv, event->function,
					    event->data, event->bytes);
		}
	}
	mutex_unlock(&gsp->msgq.mutex);
	switch (event->function) {
	case R615_GMCAPI_MMU_FAULT_QUEUED: {
		int ret = r615_health_faults(gsp, R615_FAULT_WORK_BUDGET);
		struct r615_message again = { .type = R615_MESSAGE_GMCAPI,
			.function = R615_GMCAPI_MMU_FAULT_QUEUED };

		/* No shadow queue was registered (default, UVM-only in the public
		 * host). Nothing to drain; RC_TRIGGERED remains the fault path.
		 */
		if (ret == -ENODEV) {
			nvkm_error(&gsp->subdev, "r615: MMU_FAULT_QUEUED without shadow queue, ignored\n");
			return 0;
		}
		if (ret == R615_FAULT_WORK_BUDGET)
			return r615_runtime_queue(gsp->r615_runtime, &again);
		return ret < 0 ? ret : 0;
	}
	case R615_EVENT_FECS:
		return r615_health_fecs(gsp, (const void *)event->data);
	case R615_EVENT_RECOVERY: {
		const struct r615_recovery_event *recovery = (const void *)event->data;

		return r615_health_recovery(gsp, recovery->type, recovery->value);
	}
	case R615_EVENT_POST:
		return r615_runtime_post(gsp, event->data, event->bytes);
	case R615_EVENT_RC:
		return r615_runtime_rc(gsp, (const void *)event->data);
	case R615_EVENT_ERROR: {
		const struct r615_error_event *error = (void *)event->data;

		if (event->bytes != sizeof(*error))
			return -EPROTO;
		nvkm_error(&gsp->subdev, "r615: Xid %u channel %u: %.*s\n",
			   error->xid, error->chid, (int)sizeof(error->text), error->text);
		return 0;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static void
r615_runtime_event_work(struct work_struct *work)
{
	struct r615_runtime *runtime = container_of(work, struct r615_runtime, event_work);
	struct r615_pending_event *event;
	unsigned long flags;
	u32 processed = 0;
	int ret;

	for (;;) {
		spin_lock_irqsave(&runtime->lock, flags);
		if (runtime->stopped || !runtime->started || list_empty(&runtime->events)) {
			spin_unlock_irqrestore(&runtime->lock, flags);
			break;
		}
		event = list_first_entry(&runtime->events, struct r615_pending_event, head);
		list_del(&event->head);
		runtime->queued--;
		if (event->function == R615_GMCAPI_MMU_FAULT_QUEUED)
			runtime->mmu_queued = false;
		spin_unlock_irqrestore(&runtime->lock, flags);
		ret = r615_runtime_event(runtime->gsp, event);
		if (ret)
			nvkm_error(&runtime->gsp->subdev,
				   "r615: event %x failed (%d)\n", event->function, ret);
		if (ret && (event->function == R615_GMCAPI_MMU_FAULT_QUEUED ||
			    event->function == R615_EVENT_FECS || event->function == R615_EVENT_RECOVERY))
			r615_rpc_invalidate(runtime->gsp->r615_rpc);
		kfree(event);
		if (++processed == R615_EVENT_WORK_BUDGET) {
			spin_lock_irqsave(&runtime->lock, flags);
			if (!runtime->stopped && runtime->started && !list_empty(&runtime->events))
				schedule_work(&runtime->event_work);
			spin_unlock_irqrestore(&runtime->lock, flags);
			break;
		}
	}
}

static int
r615_runtime_nocat(struct nvkm_gsp *gsp, const void *data)
{
	struct r615_nocat_event record;
	const struct r615_nocat_event *event = &record;

	memcpy(&record, data, sizeof(record));
	nvkm_error(&gsp->subdev,
		   "r615: NOCAT time=%llu type=%u bugcheck=%x subsystem=%x error=%llx tdr=%x source=%.*s engine=%.*s diag=%u\n",
		   event->timestamp, event->type, event->bugcheck, event->subsystem, event->error, event->tdr,
		   (int)sizeof(event->source), event->source, (int)sizeof(event->engine), event->engine,
		   event->diagnostic_bytes);
	for (u32 i = 0; i < event->diagnostic_bytes; i += 64) {
		u32 bytes = event->diagnostic_bytes - i;

		if (bytes > 64)
			bytes = 64;
		nvkm_error(&gsp->subdev, "r615: NOCAT diag[%u]=%*ph\n", i, (int)bytes, event->diagnostic + i);
	}
	return 0;
}

static int
r615_runtime_queue_one(void *context, const struct r615_message *message)
{
	struct r615_runtime *runtime = context;
	struct r615_pending_event *event;
	struct r615_message notification;
	unsigned long flags;
	int ret;

	/* These commands suspend GSP waiting for CPU execution. They must be
	 * serviced inline while polling, not deferred behind the current RPC.
	 */
	if (message->type != R615_MESSAGE_RPC) {
		if (message->type != R615_MESSAGE_GMCAPI || message->response ||
		    message->status_or_limit || (!message->data && message->bytes))
			return -EPROTO;
		if (READ_ONCE(runtime->stopped))
			return -ESHUTDOWN;
		if (message->function == R615_GMCAPI_MMU_FAULT_QUEUED) {
			if (message->bytes)
				return -EPROTO;
			notification = *message;
			message = &notification;
			goto enqueue; /* consume only after static handles / shadow registration */
		}
		if (message->function != R615_HOST_GENERIC && message->function != R615_HOST_HS)
			return -EOPNOTSUPP;
		return r615_host_command(runtime->gsp, message->function, message->data, message->bytes);
	}
	if (message->bytes > R615_MESSAGE_MAX_BYTES)
		return -EMSGSIZE;
	ret = r615_event_validate(message->function, message->data, message->bytes);
	if (ret == -EOPNOTSUPP && !r615_event_native(message->function)) {
		struct nvkm_gsp *gsp = runtime->gsp;

		mutex_lock(&gsp->msgq.mutex);
		for (int i = 0; i < gsp->msgq.ntfy_nr; i++) {
			if (gsp->msgq.ntfy[i].fn == message->function && gsp->msgq.ntfy[i].func) {
				ret = 0;
				break;
			}
		}
		mutex_unlock(&gsp->msgq.mutex);
	}
	/* Public kernel_gsp.c logs and ignores unexpected events. Do the same for
	 * IDs inside the public 615 event enum that nouveau neither consumes nor
	 * registers; one unconsumed notification must not poison GSP_INIT. Native
	 * IDs and IDs outside the enum (possible framing mismatch) stay strict.
	 */
	if (ret == -EOPNOTSUPP && !r615_event_native(message->function) &&
	    message->function > R615_EVENT_PUBLIC_FIRST &&
	    message->function < R615_EVENT_PUBLIC_END) {
		u64 bit = 1ULL << (message->function - R615_EVENT_PUBLIC_FIRST);
		bool first;

		spin_lock_irqsave(&runtime->lock, flags);
		runtime->dropped++;
		first = !(runtime->dropped_seen & bit);
		runtime->dropped_seen |= bit;
		spin_unlock_irqrestore(&runtime->lock, flags);
		if (first)
			nvkm_warn(&runtime->gsp->subdev,
				  "r615: event %04x (%u bytes) has no consumer, dropped\n",
				  message->function, message->bytes);
		return 0;
	}
	if (ret)
		return ret;
	if (READ_ONCE(runtime->stopped))
		return -ESHUTDOWN;
	if (message->function == R615_EVENT_LIBOS_PRINT) {
		u64 events, put;

		spin_lock_irqsave(&runtime->lock, flags);
		ret = r615_libos_update(&runtime->libos, message->data, message->bytes);
		events = runtime->libos.events;
		put = runtime->libos.put;
		spin_unlock_irqrestore(&runtime->lock, flags);
		if (!ret)
			nvkm_debug(&runtime->gsp->subdev, "r615: PMU LibOS raw snapshot event=%llu put_words=%llu\n", events, put);
		return ret;
	}
	if (message->function == R615_EVENT_BUGCHECK) {
		u32 code;

		memcpy(&code, message->data, sizeof(code));
		nvkm_error(&runtime->gsp->subdev, "r615: firmware bugcheck request %x, RPC disabled (no host panic)\n", code);
		return -EIO;
	}
	if (message->function == R615_EVENT_RECOVERY) {
		struct r615_recovery_event recovery;

		memcpy(&recovery, message->data, sizeof(recovery));
		if (recovery.type && recovery.value)
			WRITE_ONCE(runtime->gsp->r615_recovery_required, true);
		/* Scratch queries / scheduling controls must not recurse into RPC
		 * while RX owns its mutex. Preserve this record for the event worker.
		 */
	}
	if (message->function == R615_EVENT_MMU) {
		nvkm_error(&runtime->gsp->subdev, "r615: legacy MMU event is not the native GMCAPI contract\n");
		return -EOPNOTSUPP;
	}
	/* Fatal processor notices revoke RPC service, not DMA ownership. Returning
	 * an error causes the caller/poller to poison the session; only a later
	 * proven teardown can authorize releasing the boot owner's memory.
	 */
	if (message->function == R615_EVENT_PMU_HALTED || message->function == R615_EVENT_FORCED_SHUTDOWN) {
		nvkm_error(&runtime->gsp->subdev, "r615: fatal firmware event %x, RPC disabled\n", message->function);
		return -EIO;
	}
	/* Log the complete bounded diagnostic before init can fail and discard the
	 * pre-runtime event queue. This handler performs no RPC or journal pointer
	 * dereference; it is the nouveau log sink, not NVIDIA's global RC journal.
	 */
	if (message->function == R615_EVENT_NOCAT)
		return r615_runtime_nocat(runtime->gsp, message->data);
	/* Public 615 lockdown notice is processor state, not a client callback.
	 * Apply inline even before runtime_start; IRQ registers may already be
	 * inaccessible by the time deferred event_work could run.
	 */
	if (message->function == R615_EVENT_LOCKDOWN) {
		if (READ_ONCE(runtime->stopped))
			return -ESHUTDOWN;
		WRITE_ONCE(runtime->lockdown, !!*(const u8 *)message->data);
		return 0;
	}
enqueue:
	event = kmalloc(sizeof(*event) + message->bytes, GFP_KERNEL);
	if (!event)
		return -ENOMEM;
	event->function = message->function;
	event->bytes = message->bytes;
	if (message->bytes)
		memcpy(event->data, message->data, message->bytes);
	spin_lock_irqsave(&runtime->lock, flags);
	if (message->function == R615_GMCAPI_MMU_FAULT_QUEUED && runtime->mmu_queued) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		kfree(event);
		return 0;
	}
	if (runtime->stopped || runtime->queued == R615_EVENT_QUEUE_LIMIT) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		kfree(event);
		return -ENOSPC;
	}
	list_add_tail(&event->head, &runtime->events);
	runtime->queued++;
	if (message->function == R615_GMCAPI_MMU_FAULT_QUEUED)
		runtime->mmu_queued = true;
	/* Schedule while holding lock so stop cannot miss a new work item. */
	if (runtime->started)
		schedule_work(&runtime->event_work);
	spin_unlock_irqrestore(&runtime->lock, flags);
	return 0;
}

/* Every non-zero return here makes the RPC caller poison the session. Record
 * the rejected envelope first so a failed diagnostic boot names its cause.
 * Bounded to the first 32 payload bytes; copied frame, not firmware DMA.
 */
static int
r615_runtime_queue(void *context, const struct r615_message *message)
{
	struct r615_runtime *runtime = context;
	int ret = r615_runtime_queue_one(context, message);

	if (ret && ret != -ESHUTDOWN) {
		u32 bytes = message->data ? min_t(u32, message->bytes, 32) : 0;

		nvkm_error(&runtime->gsp->subdev,
			   "r615: rejected message type=%u function=%x seq=%llu bytes=%u (%d): %*ph\n",
			   message->type, message->function, (unsigned long long)message->sequence, message->bytes,
			   ret, (int)bytes, message->data);
	}
	return ret;
}

u64
r615_runtime_dropped(struct nvkm_gsp *gsp)
{
	struct r615_runtime *runtime = gsp->r615_runtime;
	unsigned long flags;
	u64 dropped;

	if (!runtime)
		return 0;
	spin_lock_irqsave(&runtime->lock, flags);
	dropped = runtime->dropped;
	spin_unlock_irqrestore(&runtime->lock, flags);
	return dropped;
}

static void
r615_runtime_rx_work(struct work_struct *work)
{
	struct r615_runtime *runtime = container_of(work, struct r615_runtime, rx_work);
	int ret;

	if (READ_ONCE(runtime->stopped) || READ_ONCE(runtime->pm_paused))
		return;
	ret = r615_rpc_poll(runtime->gsp->r615_rpc, 128);
	if (ret < 0)
		nvkm_error(&runtime->gsp->subdev, "r615: RX worker failed (%d)\n", ret);
	else if (ret == 128) {
		unsigned long flags;

		spin_lock_irqsave(&runtime->lock, flags);
		if (!runtime->stopped && !runtime->pm_paused)
			schedule_work(&runtime->rx_work);
		spin_unlock_irqrestore(&runtime->lock, flags);
	}
}

static irqreturn_t
r615_runtime_intr(struct nvkm_inth *inth)
{
	struct r615_runtime *runtime = container_of(inth, struct r615_runtime, inth);
	struct nvkm_falcon *falcon = &runtime->gsp->falcon;
	u32 raw, mask, status;

	if (READ_ONCE(runtime->lockdown) || READ_ONCE(runtime->pm_paused))
		return IRQ_NONE;
	raw = nvkm_falcon_rd32(falcon, 0x008);
	mask = nvkm_falcon_rd32(falcon, falcon->func->addr2 + falcon->func->riscv_irqmask);
	status = raw & mask;

	if (!status || raw == ~0U || mask == ~0U)
		return IRQ_NONE;
	nvkm_falcon_wr32(falcon, 0x004, status);
	if ((status & 0x40) && !READ_ONCE(runtime->stopped))
		schedule_work(&runtime->rx_work);
	if (status & ~0x40U) {
		nvkm_falcon_wr32(falcon, 0x014, status & ~0x40U);
		nvkm_error(&runtime->gsp->subdev, "r615: unexpected IRQ %08x\n", status & ~0x40U);
	}
	nvkm_falcon_intr_retrigger(falcon);
	return IRQ_HANDLED;
}

int
r615_runtime_log_copy(struct nvkm_gsp *gsp, void *output, u32 limit, u32 *bytes)
{
	struct r615_runtime *runtime = gsp->r615_runtime;
	unsigned long flags;
	int ret;

	if (!runtime)
		return -ENOENT;
	/* Caller serializes lifecycle destruction. Payload is already CPU-owned;
	 * neither firmware DMA nor queue pointers are exposed to the reader.
	 */
	spin_lock_irqsave(&runtime->lock, flags);
	ret = r615_libos_copy(&runtime->libos, output, limit, bytes);
	spin_unlock_irqrestore(&runtime->lock, flags);
	return ret;
}

int
r615_runtime_new(struct nvkm_gsp *gsp)
{
	struct r615_runtime *runtime;

	if (gsp->r615_runtime || !gsp->r615_rpc)
		return -EINVAL;
	runtime = kzalloc(sizeof(*runtime), GFP_KERNEL);
	if (!runtime)
		return -ENOMEM;
	runtime->gsp = gsp;
	spin_lock_init(&runtime->lock);
	INIT_LIST_HEAD(&runtime->events);
	INIT_WORK(&runtime->rx_work, r615_runtime_rx_work);
	INIT_WORK(&runtime->event_work, r615_runtime_event_work);
	gsp->r615_runtime = runtime;
	r615_rpc_set_dispatch(gsp->r615_rpc, r615_runtime_queue, runtime);
	return 0;
}

int
r615_runtime_start(struct nvkm_gsp *gsp)
{
	struct r615_runtime *runtime = gsp->r615_runtime;
	struct nvkm_device *device = gsp->subdev.device;
	struct r615_intr_table *table;
	int ret, vector;
	unsigned long flags;

	if (!runtime || !device->vfn)
		return -EINVAL;
	if (runtime->registered)
		return -EALREADY;
	if (READ_ONCE(runtime->stopped))
		return -ESHUTDOWN;
	table = nvkm_gsp_rm_ctrl_rd(&gsp->internal.device.subdevice,
				  R615_INTR_GET_TABLE, sizeof(*table));
	if (IS_ERR(table))
		return PTR_ERR(table);
	ret = r615_intr_table_validate(table, device->vfn->intr.leaves * 32);
	if (ret)
		goto done;
	gsp->intr_nr = 0;
	for (u32 i = 0; i < table->count; i++) {
		enum nvkm_subdev_type type;
		int instance;

		if (!gsp->rm->api->gsp->xlat_mc_engine_idx(table->entries[i].engine, &type, &instance))
			continue;
		if (gsp->intr_nr == ARRAY_SIZE(gsp->intr)) {
			ret = -ENOSPC;
			goto done;
		}
		gsp->intr[gsp->intr_nr].type = type;
		gsp->intr[gsp->intr_nr].inst = instance;
		gsp->intr[gsp->intr_nr].stall = table->entries[i].stall;
		gsp->intr[gsp->intr_nr++].nonstall = table->entries[i].nonstall;
	}
	vector = nvkm_gsp_intr_stall(gsp, NVKM_SUBDEV_GSP, 0);
	if (vector < 0) {
		ret = vector;
		goto done;
	}
	ret = nvkm_inth_add(&device->vfn->intr, vector, NVKM_INTR_PRIO_NORMAL,
			    &gsp->subdev, r615_runtime_intr, &runtime->inth);
	if (ret)
		goto done;
	runtime->registered = true;
	spin_lock_irqsave(&runtime->lock, flags);
	runtime->started = true;
	schedule_work(&runtime->event_work);
	spin_unlock_irqrestore(&runtime->lock, flags);
	nvkm_inth_allow(&runtime->inth);
	/* A notification may have arrived between linking RX and enabling IRQs. */
	schedule_work(&runtime->rx_work);
done:
	nvkm_gsp_rm_ctrl_done(&gsp->internal.device.subdevice, table);
	if (ret)
		gsp->intr_nr = 0;
	return ret;
}

bool
r615_runtime_pm_clean(struct nvkm_gsp *gsp)
{
	struct r615_runtime *runtime = gsp->r615_runtime;
	struct r615_pending_event *event;
	unsigned long flags;
	bool clean;

	if (!runtime)
		return false;
	spin_lock_irqsave(&runtime->lock, flags);
	clean = runtime->pm_paused && !runtime->stopped;
	list_for_each_entry(event, &runtime->events, head) {
		if (event->function == R615_GMCAPI_MMU_FAULT_QUEUED ||
		    event->function == R615_EVENT_RECOVERY || event->function == R615_EVENT_FECS)
			clean = false;
	}
	spin_unlock_irqrestore(&runtime->lock, flags);
	return clean;
}

int
r615_runtime_pm_pause(struct nvkm_gsp *gsp)
{
	struct r615_runtime *runtime = gsp->r615_runtime;
	unsigned long flags;

	if (!runtime || !runtime->registered || READ_ONCE(runtime->stopped))
		return -ESHUTDOWN;
	spin_lock_irqsave(&runtime->lock, flags);
	if (runtime->pm_paused) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		return -EALREADY;
	}
	runtime->pm_paused = true;
	runtime->started = false;
	spin_unlock_irqrestore(&runtime->lock, flags);
	nvkm_inth_block(&runtime->inth);
	if (gsp->subdev.device->intr.alloc)
		synchronize_irq(gsp->subdev.device->intr.irq);
	cancel_work_sync(&runtime->rx_work);
	cancel_work_sync(&runtime->event_work);
	return 0;
}

int
r615_runtime_pm_resume(struct nvkm_gsp *gsp)
{
	struct r615_runtime *runtime = gsp->r615_runtime;
	unsigned long flags;

	if (!runtime || !runtime->registered || READ_ONCE(runtime->stopped))
		return -ESHUTDOWN;
	spin_lock_irqsave(&runtime->lock, flags);
	if (!runtime->pm_paused || runtime->lockdown) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		return -EBUSY;
	}
	runtime->pm_paused = false;
	runtime->started = true;
	schedule_work(&runtime->event_work);
	spin_unlock_irqrestore(&runtime->lock, flags);
	nvkm_inth_allow(&runtime->inth);
	schedule_work(&runtime->rx_work);
	return 0;
}

void
r615_runtime_stop(struct nvkm_gsp *gsp)
{
	struct r615_runtime *runtime = gsp->r615_runtime;
	unsigned long flags;

	if (!runtime)
		return;
	spin_lock_irqsave(&runtime->lock, flags);
	runtime->stopped = true;
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (runtime->registered) {
		nvkm_inth_block(&runtime->inth);
		if (gsp->subdev.device->intr.alloc)
			synchronize_irq(gsp->subdev.device->intr.irq);
	}
	cancel_work_sync(&runtime->rx_work);
	cancel_work_sync(&runtime->event_work);
}

void
r615_runtime_del(struct nvkm_gsp *gsp)
{
	struct r615_runtime *runtime = gsp->r615_runtime;
	struct r615_pending_event *event, *next;
	unsigned long flags;

	if (!runtime)
		return;
	r615_runtime_stop(gsp);
	if (runtime->registered) {
		spin_lock_irqsave(&gsp->subdev.device->intr.lock, flags);
		list_del(&runtime->inth.head);
		spin_unlock_irqrestore(&gsp->subdev.device->intr.lock, flags);
	}
	if (gsp->r615_rpc)
		r615_rpc_set_dispatch(gsp->r615_rpc, NULL, NULL);
	list_for_each_entry_safe(event, next, &runtime->events, head) {
		list_del(&event->head);
		kfree(event);
	}
	gsp->intr_nr = 0;
	gsp->r615_runtime = NULL;
	kfree(runtime);
}
