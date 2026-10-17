// SPDX-License-Identifier: MIT
/* Native flat FBSR wire producer. Does not save BAR tables or authorize suspend. */
#include <linux/errno.h>
#include <linux/string.h>
#include <core/device.h>
#include <rm/rm.h>
#include "rpc.h"
#include "fbsr.h"

static bool
r615_fbsr_compatible(struct nvkm_gsp *gsp)
{
	return gsp && gsp->rm && gsp->rm->version && !strcmp(gsp->rm->version, "615.78.08") &&
	       gsp->subdev.device && gsp->subdev.device->chipset == 0x177;
}

int
r615_fbsr_memlist(struct nvkm_gsp_device *device, u32 memory, u32 space, u64 bytes,
		  const u64 *pages, u32 page_count, struct nvkm_gsp_object *object, bool *submitted)
{
	struct nvkm_gsp *gsp;
	struct r615_memlist_config config;
	void *request;
	u32 size;
	int ret;

	if (submitted)
		*submitted = false;
	if (!device || !device->object.client || !object || object->client || !submitted)
		return -EINVAL;
	gsp = device->object.client->gsp;
	if (!r615_fbsr_compatible(gsp))
		return -EOPNOTSUPP;
	config = (struct r615_memlist_config) {
		.client = device->object.client->object.handle, .device = device->object.handle,
		.memory = memory, .space = space, .bytes = bytes, .pages = pages, .page_count = page_count,
	};
	ret = r615_memlist_size(&config, &size);
	if (ret)
		return ret;
	if (space == R615_MEMLIST_VRAM && (pages[0] > gsp->fb.size || bytes > gsp->fb.size - pages[0]))
		return -EINVAL;
	request = nvkm_gsp_rpc_get(gsp, R615_ALLOC_MEMORY, size);
	if (IS_ERR(request))
		return PTR_ERR(request);
	ret = r615_memlist_build(&config, request, size);
	if (ret) {
		nvkm_gsp_rpc_done(gsp, request);
		return ret;
	}
	object->client = device->object.client;
	object->parent = &device->object;
	object->handle = memory;
	*submitted = true;
	/* Public _issuePteDescRpc is a large one-way payload with one status
	 * acknowledgment, not a bidirectional response echoing every PTE.
	 */
	return nvkm_gsp_rpc_wr(gsp, request, NVKM_GSP_RPC_REPLY_POLL);
}

int
r615_fbsr_memlist_free(struct nvkm_gsp_object *object)
{
	struct nvkm_gsp *gsp;
	int ret;

	if (!object || !object->client)
		return 0;
	gsp = object->client->gsp;
	if (!r615_fbsr_compatible(gsp))
		return -EOPNOTSUPP;
	ret = gsp->rm->api->alloc->free(object);
	if (!ret || ret == -ENOENT) {
		object->client = NULL;
		return 0;
	}
	return ret;
}

int
r615_fbsr_init(struct nvkm_gsp *gsp, u32 client, u32 sysmem, u64 sr_metadata, bool *submitted)
{
	struct r615_fbsr_init *request;

	if (!gsp || !submitted)
		return -EINVAL;
	*submitted = false;
	if (!r615_fbsr_compatible(gsp))
		return -EOPNOTSUPP;
	if (!client || !sysmem || !sr_metadata || sr_metadata % 4096 ||
	    sr_metadata > (1ULL << 49) - 4096)
		return -EINVAL;
	request = nvkm_gsp_rm_ctrl_get(&gsp->internal.device.subdevice, R615_FBSR_INIT, sizeof(*request));
	if (IS_ERR(request))
		return PTR_ERR(request);
	request->client = client;
	request->sysmem = sysmem;
	request->gcoff = 0; /* Standard system SR, not GC6. */
	request->sr_metadata = sr_metadata;
	*submitted = true;
	return nvkm_gsp_rm_ctrl_wr(&gsp->internal.device.subdevice, request);
}

int
r615_fbsr_channels(struct nvkm_gsp *gsp, bool suspend)
{
	u8 *request;

	if (!r615_fbsr_compatible(gsp))
		return -EOPNOTSUPP;
	request = nvkm_gsp_rm_ctrl_get(&gsp->internal.device.subdevice, R615_FBSR_CHANNELS, sizeof(*request));
	if (IS_ERR(request))
		return PTR_ERR(request);
	*request = suspend;
	return nvkm_gsp_rm_ctrl_wr(&gsp->internal.device.subdevice, request);
}
