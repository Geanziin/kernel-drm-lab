// SPDX-License-Identifier: MIT
/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved. */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <rm/rm.h>

#include "message.h"
#include "rpc.h"
#include "transport.h"
#include "rmapi.h"

#define R615_RPC_TIMEOUT_MS 10000u
#define R615_RPC_POLL_MS 1u
/* GSP usually answers in tens of microseconds; msleep(1) costs 1-2 ms per
 * poll, which dominated channel/object creation.  Poll finely for the first
 * ~2 ms of a wait, then fall back to msleep to stay cheap on slow replies.
 */
#define R615_RPC_FAST_POLLS 100u
#define R615_RPC_FAST_US_MIN 10u
#define R615_RPC_FAST_US_MAX 20u
#define R615_RPC_CHUNK_BYTES (R615_MESSAGE_MAX_BYTES - 56U)

struct r615_rpc {
	struct r615_transport *transport;
	u64 sequence;
	void *frame;
	struct mutex mutex;
	r615_rpc_dispatch_func dispatch;
	void *dispatch_context;
	bool poisoned;
	bool pm_pending, resume_waiting, resume_done;
	u64 resume_flags;
	struct task_struct *pm_owner;
	unsigned long deadline;
};

int
r615_rpc_new(struct r615_transport *transport, struct r615_rpc **output)
{
	struct r615_rpc *rpc;

	if (!transport || !output || *output)
		return -EINVAL;
	rpc = kzalloc_obj(*rpc, GFP_KERNEL);
	if (!rpc)
		return -ENOMEM;
	rpc->frame = kvzalloc(R615_MESSAGE_MAX_BYTES, GFP_KERNEL);
	if (!rpc->frame) {
		kfree(rpc);
		return -ENOMEM;
	}
	rpc->transport = transport;
	mutex_init(&rpc->mutex);
	rpc->sequence = 1;
	*output = rpc;
	return 0;
}

void
r615_rpc_del(struct r615_rpc **output)
{
	if (!output || !*output)
		return;
	mutex_destroy(&(*output)->mutex);
	kvfree((*output)->frame);
	kfree(*output);
	*output = NULL;
}

void
r615_rpc_invalidate(struct r615_rpc *rpc)
{
	mutex_lock(&rpc->mutex);
	rpc->poisoned = true;
	mutex_unlock(&rpc->mutex);
}

int
r615_rpc_pm_enter(struct r615_rpc *rpc)
{
	int ret = 0;

	if (!rpc)
		return -EINVAL;
	mutex_lock(&rpc->mutex);
	if (rpc->poisoned || rpc->pm_owner)
		ret = rpc->poisoned ? -EIO : -EBUSY;
	else
		rpc->pm_owner = current;
	mutex_unlock(&rpc->mutex);
	return ret;
}

void
r615_rpc_pm_exit(struct r615_rpc *rpc)
{
	mutex_lock(&rpc->mutex);
	if (rpc->pm_owner == current)
		rpc->pm_owner = NULL;
	mutex_unlock(&rpc->mutex);
}

void
r615_rpc_set_dispatch(struct r615_rpc *rpc, r615_rpc_dispatch_func dispatch, void *context)
{
	mutex_lock(&rpc->mutex);
	rpc->dispatch = dispatch;
	rpc->dispatch_context = context;
	mutex_unlock(&rpc->mutex);
}

static int
r615_rpc_unmatched(struct r615_rpc *rpc, const struct r615_message *message)
{
	bool event = message->type == R615_MESSAGE_RPC && message->function >= 0x1000;
	bool command = message->type == R615_MESSAGE_GMCAPI && !message->response;

	/* GMCAPI_KERNEL fire-and-forget event, not a correlated command reply.
	 * Apply under the transaction lock even if IRQ work drains it before wait.
	 */
	if (message->type == R615_MESSAGE_GMCAPI &&
	    message->function == R615_GMCAPI_GSP_RESUME_DONE) {
		u64 flags;

		if (!command || !rpc->resume_waiting || rpc->resume_done ||
		    message->status_or_limit || message->bytes != sizeof(flags) || !message->data)
			return -EPROTO;
		memcpy(&flags, message->data, sizeof(flags));
		if (flags & ~1ULL)
			return -EPROTO;
		rpc->resume_flags = flags;
		rpc->resume_done = true;
		return 0;
	}

	/* Stale/async RPC replies are not events. Never reinterpret their payload. */
	if (!event && !command)
		return 0;
	if (!rpc->dispatch)
		return -EOPNOTSUPP;
	return rpc->dispatch(rpc->dispatch_context, message);
}

/* Receive under the transaction lock. Any failure other than "empty" poisons
 * the session in the caller, so log the staged frame header first: outer
 * envelope (24 bytes) + RPC/GMCAPI header + start of payload.
 */
static int
r615_rpc_receive(struct r615_rpc *rpc, struct r615_message *message, const char *where)
{
	int ret = r615_transport_receive(rpc->transport, rpc->frame,
					 R615_MESSAGE_MAX_BYTES, message);

	if (ret && ret != -EAGAIN)
		pr_err("nvgd r615: receive failed (%d) in %s, frame: %*ph\n",
		       ret, where, 96, rpc->frame);
	return ret;
}

/* The transaction owns mutex/frame. Retry only definite queue backpressure. */
static int
r615_rpc_send(struct r615_rpc *rpc, const struct r615_message *request)
{
	unsigned long deadline = rpc->deadline;
	int ret;

	do {
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		ret = r615_transport_send(rpc->transport, request);
		if (ret != -EAGAIN)
			return ret;
		msleep(R615_RPC_POLL_MS);
	} while (!time_after_eq(jiffies, deadline));
	return -ETIMEDOUT;
}

int
r615_rpc_poll(struct r615_rpc *rpc, u32 budget)
{
	struct r615_message message;
	u32 count = 0;
	int ret = 0;

	if (!rpc || !budget || budget > 256)
		return -EINVAL;
	mutex_lock(&rpc->mutex);
	if (rpc->poisoned) {
		mutex_unlock(&rpc->mutex);
		return -EIO;
	}
	if (rpc->pm_owner && rpc->pm_owner != current) {
		mutex_unlock(&rpc->mutex);
		return -ESHUTDOWN;
	}
	if (rpc->pm_pending && !rpc->resume_waiting) {
		mutex_unlock(&rpc->mutex);
		return -ESHUTDOWN;
	}
	while (count < budget) {
		ret = r615_rpc_receive(rpc, &message, "poll");
		if (ret == -EAGAIN) {
			ret = 0;
			break;
		}
		if (ret)
			break;
		ret = r615_rpc_unmatched(rpc, &message);
		if (ret)
			break;
		count++;
	}
	if (ret)
		rpc->poisoned = true;
	mutex_unlock(&rpc->mutex);
	return ret ? ret : (int)count;
}

/* Public 615 gmcapi_gsp.h / table: fire-and-forget; only D3/hibernate here.
 * GC6, force and fast unload need separate lifecycle handling.
 */
int
r615_rpc_gsp_suspend_flags(struct r615_rpc *rpc, u64 flags)
{
	int ret;
	struct r615_message req = {
		.type = R615_MESSAGE_GMCAPI,
		.function = R615_GMCAPI_GSP_SUSPEND,
		.data = &flags,
		.bytes = sizeof(flags),
	};

	if (!rpc)
		return -EINVAL;
	if (flags & ~(R615_SUSPEND_PM_TRANSITION | R615_SUSPEND_HIBERNATE))
		return -EOPNOTSUPP;
	if ((flags & R615_SUSPEND_HIBERNATE) && !(flags & R615_SUSPEND_PM_TRANSITION))
		return -EINVAL;
	mutex_lock(&rpc->mutex);
	if (rpc->pm_owner && rpc->pm_owner != current) {
		ret = -ESHUTDOWN;
		goto done;
	}
	if (rpc->poisoned || rpc->pm_pending) {
		ret = rpc->poisoned ? -EIO : -EALREADY;
		goto done;
	}
	rpc->deadline = jiffies + msecs_to_jiffies(R615_RPC_TIMEOUT_MS);
	req.sequence = rpc->sequence++;
	ret = r615_rpc_send(rpc, &req);
	if (!ret && (flags & R615_SUSPEND_PM_TRANSITION))
		rpc->pm_pending = true;
	else if (ret && (flags & R615_SUSPEND_PM_TRANSITION))
		/* Transport errors may mean a published PM command with no known
		 * completion. Never resume normal traffic or reuse its DMA blindly.
		 */
		rpc->poisoned = true;
done:
	mutex_unlock(&rpc->mutex);
	return ret;
}

int
r615_rpc_gsp_suspend(struct r615_rpc *rpc)
{
	return r615_rpc_gsp_suspend_flags(rpc, 0);
}

int
r615_rpc_resume_begin(struct r615_rpc *rpc)
{
	int ret = 0;

	if (!rpc)
		return -EINVAL;
	mutex_lock(&rpc->mutex);
	if (rpc->poisoned)
		ret = -EIO;
	else if (rpc->pm_owner && rpc->pm_owner != current)
		ret = -ESHUTDOWN;
	else if (!rpc->pm_pending)
		ret = -EINVAL;
	else if (rpc->resume_waiting)
		ret = -EALREADY;
	else {
		rpc->resume_waiting = true;
		rpc->resume_done = false;
		rpc->resume_flags = 0;
	}
	mutex_unlock(&rpc->mutex);
	return ret;
}

int
r615_rpc_resume_wait(struct r615_rpc *rpc, bool *d3hot)
{
	struct r615_message message;
	unsigned long deadline;
	int ret = 0;

	if (!rpc || !d3hot)
		return -EINVAL;
	mutex_lock(&rpc->mutex);
	if (rpc->pm_owner && rpc->pm_owner != current) {
		ret = -ESHUTDOWN;
		goto done;
	}
	if (rpc->poisoned || !rpc->resume_waiting) {
		ret = rpc->poisoned ? -EIO : -EINVAL;
		goto done;
	}
	deadline = jiffies + msecs_to_jiffies(R615_RPC_TIMEOUT_MS);
	while (!rpc->resume_done) {
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		ret = r615_rpc_receive(rpc, &message, "resume");
		if (ret == -EAGAIN) {
			msleep(R615_RPC_POLL_MS);
			continue;
		}
		if (!ret)
			ret = r615_rpc_unmatched(rpc, &message);
		if (ret)
			break;
	}
	if (ret) {
		rpc->poisoned = true;
	} else {
		*d3hot = !!(rpc->resume_flags & 1);
		rpc->pm_pending = false;
		rpc->resume_waiting = false;
		rpc->resume_done = false;
	}
done:
	mutex_unlock(&rpc->mutex);
	return ret;
}

static int
r615_rpc_wait(struct r615_rpc *rpc, const struct r615_message *req,
	       struct r615_message *rep, u32 expect_function)
{
	unsigned long timeout = rpc->deadline;
	unsigned int polls = 0;
	int ret;

	while (true) {
		/* Check even when firmware keeps sending unrelated events. */
		if (time_after_eq(jiffies, timeout))
			return -ETIMEDOUT;
		ret = r615_rpc_receive(rpc, rep, "wait");
		if (!ret) {
			if (rep->type != req->type || rep->function != expect_function ||
			    rep->sequence != req->sequence) {
				ret = r615_rpc_unmatched(rpc, rep);
				if (ret) {
					pr_err("nvgd r615: unmatched type=%u function=%x seq=%llx while waiting for %x/%llx: %d\n",
					       rep->type, rep->function, (unsigned long long)rep->sequence,
					       expect_function, (unsigned long long)req->sequence, ret);
					rpc->poisoned = true;
					return ret;
				}
				continue;
			}
			/* VGPU RPC v3 has no response bit; only GMCAPI encodes one. */
			if (req->type == R615_MESSAGE_GMCAPI && !rep->response) {
				pr_err("nvgd r615: reply %x without response flag\n", rep->function);
				return -EPROTO;
			}
			if (rep->status_or_limit) {
				pr_err("nvgd r615: reply %x status %08x\n", rep->function, rep->status_or_limit);
				return -EIO;
			}
			return 0;
		}
		if (ret != -EAGAIN)
			return ret;
		if (polls < R615_RPC_FAST_POLLS) {
			polls++;
			usleep_range(R615_RPC_FAST_US_MIN, R615_RPC_FAST_US_MAX);
		} else {
			msleep(R615_RPC_POLL_MS);
		}
	}
}

static int
r615_rpc_exchange(struct r615_rpc *rpc, const struct r615_message *req,
		  struct r615_message *rep, u32 expect_function)
{
	int ret = r615_rpc_send(rpc, req);

	return ret ? ret : r615_rpc_wait(rpc, req, rep, expect_function);
}

int
r615_rpc_gsp_init(struct r615_rpc *rpc, const u64 *sysinfo,
		  u32 sysinfo_count, u64 *static_info,
		  u32 static_limit, u32 *static_count)
{
	struct r615_message req = { 0 }, rep = { 0 };
	int ret;
	u32 payload_bytes;

	if (!rpc || !sysinfo || !sysinfo_count || !static_info ||
	    !static_limit || !static_count)
		return -EINVAL;
	if (sysinfo_count > R615_GMCAPI_MAX_SYSINFO_WORDS ||
	    static_limit > R615_GMCAPI_MAX_STATIC_WORDS)
		return -EINVAL;

	req.type = R615_MESSAGE_GMCAPI;
	req.function = R615_GMCAPI_GSP_INIT;
	mutex_lock(&rpc->mutex);
	if (rpc->poisoned || rpc->pm_pending || rpc->pm_owner) {
		ret = rpc->poisoned ? -EIO : -ESHUTDOWN;
		goto done;
	}
	rpc->deadline = jiffies + msecs_to_jiffies(R615_RPC_TIMEOUT_MS);
	req.sequence = rpc->sequence++;
	req.status_or_limit = static_limit * sizeof(u64);
	req.data = sysinfo;
	req.bytes = sysinfo_count * sizeof(u64);

	ret = r615_rpc_exchange(rpc, &req, &rep, R615_GMCAPI_GSP_INIT);
	if (ret)
		goto done;
	payload_bytes = rep.bytes;
	/* Public kgspSendInitRpcs decodes responseSize / sizeof(NvU64) words. */
	if (payload_bytes % sizeof(u64))
		pr_warn("nvgd r615: GSP_INIT reply %u bytes, not word aligned; trailing bytes ignored\n",
			payload_bytes);
	payload_bytes -= payload_bytes % sizeof(u64);
	if (payload_bytes > static_limit * sizeof(u64)) {
		pr_err("nvgd r615: GSP_INIT reply %u bytes exceeds limit %u\n",
		       rep.bytes, static_limit * (u32)sizeof(u64));
		ret = -EPROTO;
	} else {
		memcpy(static_info, rep.data, payload_bytes);
		*static_count = payload_bytes / sizeof(u64);
	}
done:
	mutex_unlock(&rpc->mutex);
	return ret;
}

/* Public 615 vgpu/rpc.c:_issueRpcLarge: consecutive sequence numbers, function
 * 71 on subsequent records; each record's length describes that record, not
 * the whole transfer. Bidirectional large transfers return the request size.
 */
static int
r615_rpc_transfer(struct r615_rpc *rpc, u32 function, const void *request, u32 req_bytes,
		   void *reply, u32 reply_limit, u32 *reply_bytes,
		   enum nvkm_gsp_rpc_reply_policy policy, u32 minimum)
{
	struct r615_message req = { 0 }, rep = { 0 };
	u32 offset = 0, received = 0, first_sequence, records = 0;
	u32 target = req_bytes > R615_RPC_CHUNK_BYTES && reply_limit ? req_bytes : minimum;
	int ret;

	if (rpc->poisoned)
		return -EIO;
	if (rpc->pm_pending)
		return -ESHUTDOWN;
	if (rpc->pm_owner && rpc->pm_owner != current)
		return -ESHUTDOWN;
	req.type = R615_MESSAGE_RPC;
	first_sequence = policy == NVKM_GSP_RPC_REPLY_NOSEQ ? 0 : (u32)rpc->sequence;
	do {
		req.function = records ? R615_RPC_CONTINUATION : function;
		req.sequence = policy == NVKM_GSP_RPC_REPLY_NOSEQ ? 0 : (u32)rpc->sequence++;
		req.bytes = req_bytes - offset;
		if (req.bytes > R615_RPC_CHUNK_BYTES)
			req.bytes = R615_RPC_CHUNK_BYTES;
		req.data = req.bytes ? (const u8 *)request + offset : NULL;
		ret = r615_rpc_send(rpc, &req);
		if (ret) {
			/* Never continue using a half-submitted logical request. */
			if (records)
				rpc->poisoned = true;
			return ret;
		}
		offset += req.bytes;
		records++;
	} while (offset < req_bytes);
	if (policy == NVKM_GSP_RPC_REPLY_NOSEQ || policy == NVKM_GSP_RPC_REPLY_NOWAIT) {
		*reply_bytes = 0;
		return 0;
	}
	req.function = function;
	req.sequence = first_sequence;
	for (;;) {
		ret = r615_rpc_wait(rpc, &req, &rep, req.function);
		if (ret) {
			if (records > 1)
				rpc->poisoned = true;
			return ret;
		}
		if (policy == NVKM_GSP_RPC_REPLY_POLL) {
			*reply_bytes = 0;
			return 0;
		}
		if (rep.bytes > reply_limit - received)
			return -ENOSPC;
		if (rep.bytes)
			memcpy((u8 *)reply + received, rep.data, rep.bytes);
		received += rep.bytes;
		if (received >= target) {
			*reply_bytes = received;
			return 0;
		}
		if (rep.bytes != R615_RPC_CHUNK_BYTES || target > reply_limit) {
			pr_err("nvgd r615: RPC %x short continuation %u/%u\n", function, received, target);
			rpc->poisoned = true;
			return -EPROTO;
		}
		req.function = R615_RPC_CONTINUATION;
		req.sequence = (u32)(req.sequence + 1);
	}
}

int
r615_rpc_call(struct r615_rpc *rpc, u32 function,
	      const void *request, u32 req_bytes,
	      void *reply, u32 reply_limit, u32 *reply_bytes)
{
	int ret;

	if (!rpc || !function || function >= 0x1000 || function == R615_RPC_CONTINUATION ||
	    (!request && req_bytes) || (!reply && reply_limit) || !reply_bytes)
		return -EINVAL;
	if (req_bytes > R615_RPC_MAX_BYTES || reply_limit > R615_RPC_MAX_BYTES)
		return -EMSGSIZE;
	mutex_lock(&rpc->mutex);
	rpc->deadline = jiffies + msecs_to_jiffies(R615_RPC_TIMEOUT_MS);
	ret = r615_rpc_transfer(rpc, function, request, req_bytes, reply, reply_limit,
				reply_bytes, NVKM_GSP_RPC_REPLY_RECV, 0);
	mutex_unlock(&rpc->mutex);
	return ret;
}

struct r615_rpc_call {
	u32 function;
	u32 bytes;
};

u32
r615_rpc_api_bytes(const void *payload)
{
	const struct r615_rpc_call *call;

	if (!payload)
		return 0;
	call = (const void *)((const u8 *)payload - sizeof(*call));
	return call->bytes;
}

static void *
r615_rpc_api_get(struct nvkm_gsp *gsp, u32 fn, u32 argc)
{
	struct r615_rpc_call *call;

	if (!gsp || !gsp->r615_rpc || fn == 0 || fn >= 0x1000 || fn == R615_RPC_CONTINUATION)
		return ERR_PTR(-EOPNOTSUPP);
	if (argc > R615_RPC_MAX_BYTES)
		return ERR_PTR(-EMSGSIZE);
	call = kvzalloc(sizeof(*call) + argc, GFP_KERNEL);
	if (!call)
		return ERR_PTR(-ENOMEM);
	call->function = fn;
	call->bytes = argc;
	return (u8 *)call + sizeof(*call);
}

static void *
r615_rpc_api_push(struct nvkm_gsp *gsp, void *argv,
		  enum nvkm_gsp_rpc_reply_policy policy, u32 repc)
{
	struct r615_rpc *rpc = gsp ? gsp->r615_rpc : NULL;
	struct r615_rpc_call *call;
	struct r615_rpc_call *reply_call = NULL;
	u32 limit = 0, received = 0;
	int ret;

	if (!rpc)
		return ERR_PTR(-EOPNOTSUPP);
	if (!argv)
		return ERR_PTR(-EINVAL);
	call = (void *)((u8 *)argv - sizeof(*call));
	mutex_lock(&rpc->mutex);
	if (READ_ONCE(gsp->r615_recovery_required) && call->function == R615_RM_ALLOC) {
		ret = -EIO;
		goto fail;
	}
	rpc->deadline = jiffies + msecs_to_jiffies(R615_RPC_TIMEOUT_MS);
	if (gsp->r615_suspend_sent && rpc->pm_owner != current) {
		ret = -ESHUTDOWN;
		goto fail;
	}
	if (policy > NVKM_GSP_RPC_REPLY_POLL || repc > R615_RPC_MAX_BYTES) {
		pr_err("nvgd r615: RPC %u rejected locally policy=%d repc=%u\n",
		       call->function, policy, repc);
		ret = -EINVAL;
		goto fail;
	}
	/* nouveau's write helper discards the reply when no return bytes were
	 * requested. Do not hand it an allocation it has no way to release.
	 */
	if (policy == NVKM_GSP_RPC_REPLY_RECV && !repc)
		policy = NVKM_GSP_RPC_REPLY_POLL;
	if (policy == NVKM_GSP_RPC_REPLY_RECV) {
		limit = call->bytes > repc ? call->bytes : repc;
		if (limit < R615_RPC_CHUNK_BYTES)
			limit = R615_RPC_CHUNK_BYTES;
		reply_call = kvzalloc(sizeof(*reply_call) + limit, GFP_KERNEL);
		if (!reply_call) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	ret = r615_rpc_transfer(rpc, call->function, argv, call->bytes,
				reply_call ? (u8 *)reply_call + sizeof(*reply_call) : NULL,
				limit, &received, policy, repc);
	if (ret)
		goto fail;
	if (reply_call) {
		reply_call->function = call->function;
		reply_call->bytes = received;
	}
	kvfree(call);
	mutex_unlock(&rpc->mutex);
	return reply_call ? (u8 *)reply_call + sizeof(*reply_call) : NULL;

fail:
	kvfree(call);
	kvfree(reply_call);
	mutex_unlock(&rpc->mutex);
	return ERR_PTR(ret);
}

static void
r615_rpc_api_done(struct nvkm_gsp *gsp, void *repv)
{
	struct r615_rpc_call *call;

	if (!repv)
		return;
	(void)gsp;
	call = (void *)((u8 *)repv - sizeof(*call));
	kvfree(call);
}

const struct nvkm_rm_api_rpc r615_rpc_api = {
	.get = r615_rpc_api_get,
	.push = r615_rpc_api_push,
	.done = r615_rpc_api_done,
};
