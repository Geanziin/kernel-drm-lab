// SPDX-License-Identifier: MIT
/* Native 615 changed display controls. Remaining nouveau host operations use
 * separately measured compatible schemas; do not route these three via r570.
 */
#include <engine/disp.h>
#include <rm/rm.h>
#include "engine_abi.h"

static int
r615_disp_static(struct nvkm_disp *disp)
{
	struct nvkm_gsp *gsp = disp->engine.subdev.device->gsp;
	struct nvkm_gsp_object *object = &gsp->internal.device.subdevice;
	struct r615_display_static *reply;

	reply = nvkm_gsp_rm_ctrl_rd(object, R615_DISPLAY_STATIC, sizeof(*reply));
	if (IS_ERR(reply))
		return PTR_ERR(reply);
	disp->wndw.mask = reply->windows;
	disp->wndw.nr = fls(reply->windows);
	nvkm_gsp_rm_ctrl_done(object, reply);
	return 0;
}

static int
r615_disp_active(struct nvkm_disp *disp, unsigned head, u32 *display)
{
	struct r615_display_active *request;
	int ret;

	request = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom, R615_DISPLAY_ACTIVE, sizeof(*request));
	if (IS_ERR(request))
		return PTR_ERR(request);
	request->head = head;
	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &request, sizeof(*request));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, request);
		return ret;
	}
	*display = request->display;
	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, request);
	return 0;
}

static int
r615_dp_caps(struct nvkm_disp *disp, int *link_bw, bool *mst, bool *watermark)
{
	struct r615_dp_caps *request;
	static const u8 rates[] = { 0, 0x06, 0x0a, 0x14, 0x1e };
	int ret;

	request = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom, R615_DP_CAPS, sizeof(*request));
	if (IS_ERR(request))
		return PTR_ERR(request);
	request->sor = ~0U;
	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &request, sizeof(*request));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, request);
		return ret;
	}
	*link_bw = (request->link_rate & 7) < ARRAY_SIZE(rates) ? rates[request->link_rate & 7] : 0;
	*mst = !!request->mst;
	*watermark = !!request->watermark;
	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, request);
	return 0;
}

static int r615_disp_supported(struct nvkm_disp *d, unsigned long *m)
{ return r570_disp.get_supported(d, m); }
static int r615_disp_connected(struct nvkm_disp *d, unsigned id)
{ return r570_disp.get_connect_state(d, id); }
static int r615_disp_brightness(struct nvkm_disp *d, unsigned id, bool set, int *v)
{ return r570_disp.bl_ctrl(d, id, set, v); }
static int r615_dp_rates(struct nvkm_outp *o)
{ return r570_disp.dp.set_indexed_link_rates(o); }
static int r615_dp_sst(struct nvkm_ior *i, int h, bool ef, u32 wm, u32 hb, u32 vb)
{ return r570_disp.dp.sst(i, h, ef, wm, hb, vb); }
static int r615_dp_vcpi(struct nvkm_ior *i, int h, u8 s, u8 n, u16 p, u16 a)
{ return r570_disp.dp.vcpi(i, h, s, n, p, a); }
static int r615_disp_pushbuf(struct nvkm_disp *d, s32 c, int i, struct nvkm_memory *m)
{ return r570_disp.chan.set_pushbuf(d, c, i, m); }
static int r615_disp_dmac(struct nvkm_disp *d, u32 c, int i, u32 p, struct nvkm_gsp_object *o)
{ return r570_disp.chan.dmac_alloc(d, c, i, p, o); }

const struct nvkm_rm_api_disp r615_disp = {
	.get_static_info = r615_disp_static, .get_active = r615_disp_active,
	.get_supported = r615_disp_supported, .get_connect_state = r615_disp_connected,
	.bl_ctrl = r615_disp_brightness,
	.dp = { .get_caps = r615_dp_caps, .set_indexed_link_rates = r615_dp_rates,
		.sst = r615_dp_sst, .vcpi = r615_dp_vcpi },
	.chan = { .set_pushbuf = r615_disp_pushbuf, .dmac_alloc = r615_disp_dmac },
};
