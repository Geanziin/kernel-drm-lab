// SPDX-License-Identifier: MIT
/* GSP-RM performance/power state (P-State) control.
 *
 * Reverse-engineered from the MIT 615 sources shipped with the proprietary
 * driver, not invented here:
 *
 *   - arch/nvalloc/unix/src/osapi.c:RmPowerSourceChangeEvent() sends
 *     NV2080_CTRL_CMD_PERF_SET_POWERSTATE on the client/subdevice handles with
 *     NV2080_CTRL_PERF_POWER_SOURCE_AC (0x0) or ..._BATTERY (0x1).  The lab
 *     carried 0x1 for "AC": RM therefore stayed on the battery profile.
 *   - osapi.c:RmHandleDNotifierEvent() answers ACPI D-Notifiers with
 *     NV2080_CTRL_CMD_PERF_SET_AUX_POWER_STATE, D1 -> AUX_POWER_STATE_P0
 *     (0x0), D5 -> P4.  Forcing P0 is the driver's own "maximum auxiliary
 *     power" request; the previous lab code used the value by luck but never
 *     checked the status and re-issued it from every GSP device allocation.
 *   - ctrl/ctrl2080/ctrl2080perf.h (public): both parameter structures are a
 *     single NvU32, and NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE returns the
 *     NV2080_CTRL_PERF_PSTATES_* bit of the level RM has arbitrated.
 *
 * The controls are dispatched through the active RM API (r535's framed RPC or
 * the native 615 flat control), so this file carries no wire layout of its own.
 */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/types.h>

#include <core/device.h>
#include <subdev/gsp.h>

#include "perf.h"

#define NVKM_GSP_PERF_CMD_SET_POWERSTATE      0x2080205bU
#define NVKM_GSP_PERF_CMD_GET_POWERSTATE      0x2080205aU
#define NVKM_GSP_PERF_CMD_SET_AUX_POWER_STATE 0x20802092U
#define NVKM_GSP_PERF_CMD_GET_CURRENT_PSTATE  0x20802068U

#define NVKM_GSP_PERF_PSTATES_P0 0x00000001U

/* RmHandleDNotifierEvent()'s D1 level is the maximum auxiliary state.  Kept as
 * a module parameter so a battery/mobile lab can drop back to RM arbitration
 * without rebuilding; the desktop target pins P0.
 */
static bool nvkm_gsp_perf_pstate_p0 = true;
module_param_named(pstate_p0, nvkm_gsp_perf_pstate_p0, bool, 0644);
MODULE_PARM_DESC(pstate_p0,
		 "request NV2080_CTRL_PERF_AUX_POWER_STATE_P0 from GSP-RM once at boot (default: y)");

static bool
nvkm_gsp_perf_ready(struct nvkm_gsp *gsp, const char *who)
{
	if (!gsp || !gsp->rm || !gsp->rm->api || !gsp->rm->api->ctrl)
		return false;
	if (!gsp->internal.device.subdevice.handle) {
		nvkm_warn(&gsp->subdev,
			  "perf: %s without an internal subdevice handle, skipped\n", who);
		return false;
	}
	return true;
}

static int
nvkm_gsp_perf_set(struct nvkm_gsp *gsp, u32 command, u32 value, const char *what)
{
	struct nvkm_gsp_object *object = &gsp->internal.device.subdevice;
	struct {
		u32 value;
	} *params;
	int ret;

	params = nvkm_gsp_rm_ctrl_get(object, command, sizeof(*params));
	if (IS_ERR(params))
		return PTR_ERR(params);
	params->value = value;
	ret = nvkm_gsp_rm_ctrl_wr(object, params);
	if (ret)
		nvkm_warn(&gsp->subdev, "perf: %s value=%u failed (%d)\n",
			  what, value, ret);
	return ret;
}

int
nvkm_gsp_perf_current_pstate(struct nvkm_gsp *gsp, u32 *pstate)
{
	struct nvkm_gsp_object *object = &gsp->internal.device.subdevice;
	struct {
		u32 currPstate;
	} *params;

	if (!nvkm_gsp_perf_ready(gsp, "GET_CURRENT_PSTATE"))
		return -ENODEV;
	params = nvkm_gsp_rm_ctrl_rd(object, NVKM_GSP_PERF_CMD_GET_CURRENT_PSTATE,
				     sizeof(*params));
	if (IS_ERR(params))
		return PTR_ERR(params);
	*pstate = params->currPstate;
	nvkm_gsp_rm_ctrl_done(object, params);
	return 0;
}

int
nvkm_gsp_perf_max_performance(struct nvkm_gsp *gsp)
{
	u32 pstate = 0;
	int ret;

	if (!nvkm_gsp_perf_ready(gsp, "max_performance"))
		return -ENODEV;

	/* AC power source first: on a desktop this is the state the SBIOS
	 * reports anyway, and RM refuses aux forcing while it believes the
	 * part runs on battery.
	 */
	ret = nvkm_gsp_perf_set(gsp, NVKM_GSP_PERF_CMD_SET_POWERSTATE,
				NVKM_GSP_PERF_POWER_SOURCE_AC, "SET_POWERSTATE(AC)");
	if (ret)
		return ret;

	if (!READ_ONCE(nvkm_gsp_perf_pstate_p0)) {
		nvkm_info(&gsp->subdev, "perf: P0 request disabled, RM arbitrates\n");
		return 0;
	}

	ret = nvkm_gsp_perf_set(gsp, NVKM_GSP_PERF_CMD_SET_AUX_POWER_STATE,
				NVKM_GSP_PERF_AUX_POWER_STATE_P0, "SET_AUX_POWER_STATE(P0)");
	if (ret)
		return ret;

	if (!nvkm_gsp_perf_current_pstate(gsp, &pstate)) {
		nvkm_info(&gsp->subdev,
			  "perf: P0 requested, current P-State mask=%08x\n", pstate);
		if (!(pstate & NVKM_GSP_PERF_PSTATES_P0))
			nvkm_info(&gsp->subdev,
				  "perf: RM has not selected P0 yet (mask=%08x)\n", pstate);
	} else {
		nvkm_info(&gsp->subdev, "perf: P0 requested\n");
	}

	return 0;
}
