// SPDX-License-Identifier: MIT
/* Native 615 channel producer. Unchanged host translation retained only where
 * wire IDs/layouts are checked against the public 615 SDK, not by version name.
 */
#include <engine/fifo/priv.h>
#include <engine/fifo/chan.h>
#include <engine/fifo/runl.h>
#include <subdev/mmu.h>
#include <rm/rm.h>
#include "engine_abi.h"

static int
r615_chan_alloc(struct nvkm_gsp_device *device, u32 handle, u32 engine, u8 runqueue,
		bool privileged, int chid, u64 instance, u64 userd, u64 methods,
		struct nvkm_vmm *vmm, u64 gpfifo, u32 gpfifo_bytes, struct nvkm_gsp_object *chan)
{
	struct nvkm_gsp *gsp = device->object.client->gsp;
	struct nvkm_fifo *fifo = gsp->subdev.device->fifo;
	struct r615_channel_alloc built, *request;
	struct r615_channel_config config = {
		.gpfifo = gpfifo, .gpfifo_bytes = gpfifo_bytes, .instance = instance,
		.instance_bytes = fifo->func->chan.func->inst->size,
		.userd = userd, .userd_bytes = fifo->func->chan.func->userd->size,
		.methods = methods, .methods_bytes = fifo->rm.mthdbuf_size,
		.vaspace = vmm->rm.object.handle, .engine = engine, .chid = chid,
		.runqueue = runqueue, .privileged = privileged,
	};
	int ret = r615_channel_build(&config, &built);

	if (ret) {
		nvkm_error(&gsp->subdev,
			   "r615: channel build rejected (%d): chid=%u runq=%u priv=%d engine=%u "
			   "gpfifo=%llx/%x inst=%llx/%x userd=%llx/%x mthd=%llx/%x vas=%08x\n",
			   ret, config.chid, config.runqueue, config.privileged, config.engine,
			   config.gpfifo, config.gpfifo_bytes, config.instance, config.instance_bytes,
			   config.userd, config.userd_bytes, config.methods, config.methods_bytes,
			   config.vaspace);
		return ret;
	}
	request = nvkm_gsp_rm_alloc_get(&device->object, handle,
				       fifo->func->chan.user.oclass, sizeof(*request), chan);
	if (IS_ERR(request))
		return PTR_ERR(request);
	*request = built;
	ret = nvkm_gsp_rm_alloc_wr(chan, request);
	if (ret)
		nvkm_error(&gsp->subdev, "r615: channel alloc class %04x chid=%d engine=%u failed (%d)\n",
			   fifo->func->chan.user.oclass, chid, engine, ret);
	return ret;
}

static int
r615_fifo_xlat(u32 engine, enum nvkm_subdev_type *type, int *nv2080)
{
	return r570_fifo.xlat_rm_engine_type(engine, type, nv2080);
}

static int
r615_fifo_ectx_size(struct nvkm_fifo *fifo)
{
	/* Public ctrl2080gpu.h: max 64, 20-byte constructed Falcon entries. */
	struct { u32 count; struct { u32 desc, attr, bytes, spaces, base; } entry[64]; } *reply;
	struct nvkm_gsp *gsp = fifo->engine.subdev.device->gsp;
	struct nvkm_gsp_object *object = &gsp->internal.device.subdevice;
	struct nvkm_runl *runl;
	struct nvkm_engn *engn;

	reply = nvkm_gsp_rm_ctrl_rd(object, 0x208001b0U, sizeof(*reply));
	if (IS_ERR(reply))
		return PTR_ERR(reply);
	if (reply->count > ARRAY_SIZE(reply->entry)) {
		nvkm_gsp_rm_ctrl_done(object, reply);
		return -EPROTO;
	}
	for (u32 i = 0; i < reply->count; i++) {
		nvkm_runl_foreach(runl, fifo) {
			nvkm_runl_foreach_engn(engn, runl) {
				if (engn->rm.desc == reply->entry[i].desc)
					engn->rm.size = reply->entry[i].bytes;
			}
		}
	}
	nvkm_gsp_rm_ctrl_done(object, reply);
	return 0;
}

/* Boot 15: the r535 3212-byte request was refused (size check, -EINVAL).
 * Pages with baseIndex while the RM reports bMore; bounded by the caller.
 */
static int
r615_fifo_device_info(struct nvkm_gsp *gsp, struct nvkm_rm_fifo_device *out, u32 limit, u32 *count)
{
	struct nvkm_gsp_object *object = &gsp->internal.device.subdevice;
	struct r615_fifo_device_info *reply;
	u32 total = 0;
	bool more;

	do {
		reply = nvkm_gsp_rm_ctrl_get(object, R615_FIFO_DEVICE_INFO, sizeof(*reply));
		if (IS_ERR(reply))
			return PTR_ERR(reply);
		reply->base = total;
		{
			int ret = nvkm_gsp_rm_ctrl_push(object, &reply, sizeof(*reply));

			if (ret) {
				nvkm_gsp_rm_ctrl_done(object, reply);
				return ret;
			}
		}
		if (reply->count > R615_FIFO_DEVICE_ENTRIES || reply->more > 1 ||
		    reply->count > limit - total || (reply->more && !reply->count)) {
			nvkm_gsp_rm_ctrl_done(object, reply);
			return -EPROTO;
		}
		for (u32 i = 0; i < reply->count; i++) {
			const u32 *data = reply->entries[i].data;

			out[total + i] = (struct nvkm_rm_fifo_device) {
				.eng_desc = data[R615_ENGINE_INFO_ENG_DESC],
				.rm_engine_type = data[R615_ENGINE_INFO_RM_ENGINE_TYPE],
				.runlist = data[R615_ENGINE_INFO_RUNLIST],
				.runlist_pri_base = data[R615_ENGINE_INFO_RUNLIST_PRI_BASE],
			};
		}
		total += reply->count;
		more = reply->more;
		nvkm_gsp_rm_ctrl_done(object, reply);
	} while (more);
	*count = total;
	return 0;
}

const struct nvkm_rm_api_fifo r615_fifo = {
	.device_info = r615_fifo_device_info,
	.xlat_rm_engine_type = r615_fifo_xlat, .ectx_size = r615_fifo_ectx_size,
	.rsvd_chids = 1, .chan.alloc = r615_chan_alloc,
	/* RC events are handled by runtime.c's native 615 parser, not a legacy hook. */
};
