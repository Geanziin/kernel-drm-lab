// SPDX-License-Identifier: MIT
/* Native flat RM control/allocation adapters, from the public MIT 615 ABI.
 * A matching v03_00 name does not imply the old control header is compatible:
 * params start at byte 40, not byte 24. No FINN pointer serialization here.
 */
#include <linux/array_size.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <rm/rm.h>
#include "rpc.h"
#include "rmapi.h"

static_assert(sizeof(struct r615_rm_control) == 40);
static_assert(sizeof(struct r615_rm_alloc) == 32);
static_assert(sizeof(struct r615_rm_free) == 16);

int
r615_rm_status(u32 status)
{
	switch (status) {
	case 0: return 0;
	case 0x03: return -EAGAIN; /* NV_ERR_BUSY_RETRY */
	case 0x1b: return -EACCES; /* NV_ERR_INSUFFICIENT_PERMISSIONS */
	case 0x1f: return -EINVAL; /* NV_ERR_INVALID_ARGUMENT */
	case 0x31: return -ENOENT; /* NV_ERR_INVALID_OBJECT */
	case 0x38: return -EINVAL; /* NV_ERR_INVALID_OPERATION */
	case 0x3a: return -EINVAL; /* NV_ERR_INVALID_PARAM_STRUCT */
	case 0x3b: return -EINVAL; /* NV_ERR_INVALID_PARAMETER */
	case 0x40: return -EIO; /* NV_ERR_INVALID_STATE */
	case 0x51: return -ENOMEM; /* NV_ERR_NO_MEMORY */
	case 0x55: return -EBUSY; /* NV_ERR_NOT_READY */
	case 0x56: return -EOPNOTSUPP; /* NV_ERR_NOT_SUPPORTED */
	case 0x57: return -ENOENT; /* NV_ERR_OBJECT_NOT_FOUND */
	case 0x65: return -ETIMEDOUT; /* NV_ERR_TIMEOUT */
	case 0x66: return -EBUSY; /* NV_ERR_TIMEOUT_RETRY */
	default: return -EIO;
	}
}

/* Inherited controls whose 615 params only APPEND fields (prefix measured
 * equal by scripts/audit-engine-abi615.py). RM rejects any paramsSize other
 * than the descriptor's (public control.c -> NV_ERR_INVALID_ARGUMENT).
 * Boot 16: 2-byte r535 GPFIFO_SCHEDULE failed every channel with -EINVAL.
 */
static const struct {
	u32 command, legacy_bytes, bytes;
} r615_ctrl_extended[] = {
	{ 0xa06f0103, 2, 3 }, /* NVA06F GPFIFO_SCHEDULE: + bSkipEnable (false) */
};

u32
r615_ctrl_wire_bytes(u32 command, u32 bytes)
{
	for (size_t i = 0; i < ARRAY_SIZE(r615_ctrl_extended); i++) {
		if (r615_ctrl_extended[i].command == command &&
		    r615_ctrl_extended[i].legacy_bytes == bytes)
			return r615_ctrl_extended[i].bytes;
	}
	return bytes;
}

static void *
r615_ctrl_get(struct nvkm_gsp_object *object, u32 command, u32 bytes)
{
	struct nvkm_gsp *gsp = object->client->gsp;
	struct r615_rm_control *request;

	bytes = r615_ctrl_wire_bytes(command, bytes);
	if (bytes > R615_RPC_MAX_BYTES - sizeof(*request))
		return ERR_PTR(-EMSGSIZE);
	/* rpc_get zero-fills: appended 615 fields start at their default. */
	request = nvkm_gsp_rpc_get(gsp, R615_RM_CONTROL, sizeof(*request) + bytes);
	if (IS_ERR(request))
		return request;
	request->client = object->client->object.handle;
	request->object = object->handle;
	request->command = command;
	request->params_bytes = bytes;
	return request->params;
}

static void
r615_ctrl_done(struct nvkm_gsp_object *object, void *params)
{
	if (params)
		nvkm_gsp_rpc_done(object->client->gsp,
				 (u8 *)params - sizeof(struct r615_rm_control));
}

static int
r615_ctrl_push(struct nvkm_gsp_object *object, void **params, u32 repc)
{
	struct nvkm_gsp *gsp = object->client->gsp;
	struct r615_rm_control *request, *reply;
	u32 command, bytes;
	int ret;

	if (!params || !*params)
		return -EINVAL;
	request = (void *)((u8 *)*params - sizeof(*request));
	*params = NULL;
	command = request->command;
	bytes = request->params_bytes;
	if (r615_rpc_api_bytes(request) < sizeof(*request) ||
	    bytes != r615_rpc_api_bytes(request) - sizeof(*request) || repc > bytes) {
		pr_err("nvgd r615: ctrl %08x local size reject params=%u repc=%u\n", command, bytes, repc);
		nvkm_gsp_rpc_done(gsp, request);
		return -EINVAL;
	}
	reply = nvkm_gsp_rpc_push(gsp, request, NVKM_GSP_RPC_REPLY_RECV,
				 sizeof(*reply));
	if (IS_ERR_OR_NULL(reply))
		return reply ? PTR_ERR(reply) : -EIO;
	if (r615_rpc_api_bytes(reply) < sizeof(*reply) ||
	    reply->client != object->client->object.handle || reply->object != object->handle ||
	    reply->command != command || (reply->rpc_flags & 2)) {
		ret = -EPROTO;
		goto done;
	}
	if (reply->params_bytes > bytes || reply->params_bytes < repc ||
	    reply->params_bytes > r615_rpc_api_bytes(reply) - sizeof(*reply)) {
		ret = -EPROTO;
		goto done;
	}
	ret = r615_rm_status(reply->status);
	/* Callers retry some (DP AUX defer); log non-retry statuses only. */
	if (ret && ret != -EAGAIN && ret != -EBUSY)
		pr_err("nvgd r615: ctrl %08x on %08x status %08x (%d)\n",
		       command, object->handle, reply->status, ret);
	/* Shared nouveau contract (r535): with repc, the structurally valid reply
	 * is returned even on an RM status error; the caller still owns done().
	 * Boot 15: r535_dp_aux_xfer read ctrl->data after a status error -> NULL.
	 */
	if (repc) {
		*params = reply->params;
		return ret;
	}
done:
	nvkm_gsp_rpc_done(gsp, reply);
	return ret;
}

/* Inherited allocators whose 615 parameters only APPEND fields to the old
 * layout (prefix offsets measured equal by scripts/audit-engine-abi615.py).
 * RM validates paramsSize against the class descriptor, so the wire size is
 * the 615 size; appended fields are zero = public default. Boot 14: the
 * 48-byte r535 FERMI_VASPACE_A request returned NV_ERR_INVALID_PARAMETER.
 */
static const struct {
	u32 class, legacy_bytes, bytes;
} r615_alloc_extended[] = {
	{ 0x000090f1, 48, 56 }, /* FERMI_VASPACE_A: + pasid (nvos.h) */
};

u32
r615_alloc_wire_bytes(u32 class, u32 bytes)
{
	for (size_t i = 0; i < ARRAY_SIZE(r615_alloc_extended); i++) {
		if (r615_alloc_extended[i].class == class &&
		    r615_alloc_extended[i].legacy_bytes == bytes)
			return r615_alloc_extended[i].bytes;
	}
	return bytes;
}

static void *
r615_alloc_get(struct nvkm_gsp_object *object, u32 class, u32 bytes)
{
	struct nvkm_gsp *gsp = object->client->gsp;
	struct r615_rm_alloc *request;

	if (!object->parent) {
		pr_err("nvgd r615: alloc class %08x handle %08x without parent\n", class, object->handle);
		return ERR_PTR(-EINVAL);
	}
	bytes = r615_alloc_wire_bytes(class, bytes);
	if (bytes > R615_RPC_MAX_BYTES - sizeof(*request))
		return ERR_PTR(-EMSGSIZE);
	/* rpc_get zero-fills, so appended 615 fields start at their default. */
	request = nvkm_gsp_rpc_get(gsp, R615_RM_ALLOC, sizeof(*request) + bytes);
	if (IS_ERR(request))
		return request;
	request->client = object->client->object.handle;
	request->parent = object->parent->handle;
	request->object = object->handle;
	request->class = class;
	request->params_bytes = bytes;
	return request->params;
}

static void
r615_alloc_done(struct nvkm_gsp_object *object, void *params)
{
	if (params)
		nvkm_gsp_rpc_done(object->client->gsp,
				 (u8 *)params - sizeof(struct r615_rm_alloc));
}

static void *
r615_alloc_push(struct nvkm_gsp_object *object, void *params)
{
	struct nvkm_gsp *gsp = object->client->gsp;
	struct r615_rm_alloc *request, *reply;
	u32 class, bytes;
	int ret;

	if (!params)
		return ERR_PTR(-EINVAL);
	request = (void *)((u8 *)params - sizeof(*request));
	class = request->class;
	bytes = request->params_bytes;
	if (r615_rpc_api_bytes(request) < sizeof(*request) ||
	    bytes != r615_rpc_api_bytes(request) - sizeof(*request)) {
		pr_err("nvgd r615: alloc class %08x local size reject params=%u\n", class, bytes);
		nvkm_gsp_rpc_done(gsp, request);
		return ERR_PTR(-EINVAL);
	}
	reply = nvkm_gsp_rpc_push(gsp, request, NVKM_GSP_RPC_REPLY_RECV, sizeof(*reply));
	if (IS_ERR_OR_NULL(reply))
		return reply ? reply : ERR_PTR(-EIO);
	if (r615_rpc_api_bytes(reply) < sizeof(*reply) ||
	    reply->client != object->client->object.handle ||
	    reply->parent != object->parent->handle || reply->object != object->handle ||
	    reply->class != class || (reply->flags & 2)) {
		ret = -EPROTO;
	} else {
		ret = r615_rm_status(reply->status);
		if (ret)
			pr_err("nvgd r615: alloc class %08x parent %08x handle %08x status %08x (%d)\n",
			       class, object->parent->handle, object->handle, reply->status, ret);
		if (!ret && (reply->params_bytes > bytes ||
			     reply->params_bytes > r615_rpc_api_bytes(reply) - sizeof(*reply)))
			ret = -EPROTO;
	}
	/* nouveau's alloc API has no copyout consumer; success returns NULL. */
	nvkm_gsp_rpc_done(gsp, reply);
	return ret ? ERR_PTR(ret) : NULL;
}

static int
r615_alloc_free(struct nvkm_gsp_object *object)
{
	struct nvkm_gsp *gsp = object->client->gsp;
	struct r615_rm_free *request, *reply;
	int ret;

	request = nvkm_gsp_rpc_get(gsp, R615_RM_FREE, sizeof(*request));
	if (IS_ERR(request))
		return PTR_ERR(request);
	request->client = object->client->object.handle;
	request->object = object->handle;
	reply = nvkm_gsp_rpc_push(gsp, request, NVKM_GSP_RPC_REPLY_RECV, sizeof(*reply));
	if (IS_ERR_OR_NULL(reply))
		return reply ? PTR_ERR(reply) : -EIO;
	if (r615_rpc_api_bytes(reply) != sizeof(*reply) ||
	    reply->client != object->client->object.handle || reply->parent ||
	    reply->object != object->handle)
		ret = -EPROTO;
	else
		ret = r615_rm_status(reply->status);
	nvkm_gsp_rpc_done(gsp, reply);
	return ret;
}

const struct nvkm_rm_api_ctrl r615_ctrl = {
	.get = r615_ctrl_get, .push = r615_ctrl_push, .done = r615_ctrl_done,
};
const struct nvkm_rm_api_alloc r615_alloc = {
	.get = r615_alloc_get, .push = r615_alloc_push, .done = r615_alloc_done,
	.free = r615_alloc_free,
};
