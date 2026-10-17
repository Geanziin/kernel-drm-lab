/* SPDX-License-Identifier: MIT */
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <rm/rm.h>

#include "rusd_session.h"

struct r615_rusd_session {
	struct nvkm_gsp *gsp;
	struct mutex mutex;
	struct nvkm_gsp_mem memory;
	struct r615_rusd_producer producer;
};

static bool
r615_rusd_session_compatible(struct nvkm_gsp *gsp)
{
	return gsp && gsp->rm && gsp->rm->version &&
	       !strcmp(gsp->rm->version, "615.78.08") && gsp->rm->api &&
	       gsp->rm->api->ctrl && gsp->subdev.device->chipset == 0x177;
}

static int
r615_rusd_session_copy(void *context, size_t offset, void *destination, size_t bytes)
{
	struct r615_rusd_session *session = context;

	if (!session->memory.data || offset > session->memory.size ||
	    bytes > session->memory.size - offset)
		return -EINVAL;
	memcpy(destination, (u8 *)session->memory.data + offset, bytes);
	return 0;
}

static void
r615_rusd_session_barrier(void *context)
{
	dma_rmb();
}

static int
r615_rusd_session_allocate(void *context, size_t bytes, u64 *address,
			   struct r615_rusd_source *source)
{
	struct r615_rusd_session *session = context;
	int ret;

	ret = nvkm_gsp_mem_ctor(session->gsp, bytes, &session->memory);
	if (ret)
		return ret;
	memset(session->memory.data, 0, bytes);
	dma_wmb();
	*address = session->memory.addr;
	*source = (struct r615_rusd_source) {
		.context = session,
		.bytes = session->memory.size,
		.copy = r615_rusd_session_copy,
		.read_barrier = r615_rusd_session_barrier,
	};
	return 0;
}

static void
r615_rusd_session_release(void *context)
{
	struct r615_rusd_session *session = context;

	nvkm_gsp_mem_dtor(&session->memory);
}

static int
r615_rusd_session_control(void *context, u32 command, const void *params, size_t bytes)
{
	struct r615_rusd_session *session = context;
	struct nvkm_gsp *gsp = session->gsp;
	struct nvkm_gsp_object *object = &gsp->internal.device.subdevice;
	void *request;

	if (!r615_rusd_session_compatible(gsp))
		return -EOPNOTSUPP;
	if (!gsp->running || !object->client)
		return -EAGAIN;
	request = nvkm_gsp_rm_ctrl_get(object, command, bytes);
	if (IS_ERR(request))
		return PTR_ERR(request);
	if (!request)
		return -EIO;
	memcpy(request, params, bytes);
	return nvkm_gsp_rm_ctrl_wr(object, request);
}

static const struct r615_rusd_producer_ops r615_rusd_session_ops = {
	.allocate = r615_rusd_session_allocate,
	.release = r615_rusd_session_release,
	.control = r615_rusd_session_control,
};

int
r615_rusd_session_new(struct nvkm_gsp *gsp, struct r615_rusd_session **out)
{
	struct r615_rusd_session *session;

	if (!out || *out)
		return -EINVAL;
	if (!r615_rusd_session_compatible(gsp))
		return -EOPNOTSUPP;
	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session)
		return -ENOMEM;
	session->gsp = gsp;
	mutex_init(&session->mutex);
	*out = session;
	return 0;
}

int
r615_rusd_session_start(struct r615_rusd_session *session, u64 poll_mask, u32 interval_ms)
{
	int ret;

	if (!session)
		return -EINVAL;
	mutex_lock(&session->mutex);
	if (!r615_rusd_session_compatible(session->gsp))
		ret = -EOPNOTSUPP;
	else if (!session->gsp->running || !session->gsp->internal.device.subdevice.client)
		ret = -EAGAIN;
	else
		ret = r615_rusd_producer_start(&session->producer, &r615_rusd_session_ops,
					       session, poll_mask, interval_ms);
	mutex_unlock(&session->mutex);
	return ret;
}

int
r615_rusd_session_stop(struct r615_rusd_session *session)
{
	int ret;

	if (!session)
		return -EINVAL;
	mutex_lock(&session->mutex);
	ret = r615_rusd_producer_stop(&session->producer);
	mutex_unlock(&session->mutex);
	return ret;
}

int
r615_rusd_session_read(struct r615_rusd_session *session, enum r615_rusd_metric metric,
		       u64 verified_now_ns, u64 max_age_ns, struct r615_rusd_value *value)
{
	int ret;

	if (!session)
		return -EINVAL;
	mutex_lock(&session->mutex);
	if (!r615_rusd_session_compatible(session->gsp))
		ret = -EOPNOTSUPP;
	else if (!session->gsp->running)
		ret = -EAGAIN;
	else
		ret = r615_rusd_producer_read(&session->producer, metric, verified_now_ns,
					      max_age_ns, value);
	mutex_unlock(&session->mutex);
	return ret;
}

int
r615_rusd_session_quiesced(struct r615_rusd_session *session)
{
	if (!session)
		return -EINVAL;
	mutex_lock(&session->mutex);
	if (session->gsp->running) {
		mutex_unlock(&session->mutex);
		return -EBUSY;
	}
	r615_rusd_producer_quiesced(&session->producer);
	mutex_unlock(&session->mutex);
	return 0;
}

int
r615_rusd_session_del(struct r615_rusd_session **out)
{
	struct r615_rusd_session *session;
	int ret;

	if (!out || !*out)
		return 0;
	session = *out;
	ret = r615_rusd_session_stop(session);
	if (session->producer.state != R615_RUSD_STOPPED)
		return ret ? ret : -EBUSY;
	mutex_destroy(&session->mutex);
	kfree(session);
	*out = NULL;
	return ret;
}
