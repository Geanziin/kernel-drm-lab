// SPDX-License-Identifier: MIT
/*
 * NVIDIA-DERIVED: private local use only, not for redistribution/upstream.
 *
 * Origin: NVIDIA libnvidia-ml.so.615.78.08
 *   sha256 3007b7a1714c4be9e7ff4c484bed786c1da220a1f9e78bbc5b394bb2502f2d1d
 *   nvmlDeviceGetPowerManagementLimitConstraints (0x81290) -> 0x81140 ->
 *   HAL 0x10bf20 (kind 0: min -> 6th arg, max -> 7th arg), cache filled by
 *   0x102720 with control 0x2080a618 (zeroed 0x5020-byte request).
 * Derived (Ghidra decompilation): reply layout used to pick the TGP policy
 * and its limits. Only the layout/selection is derived; no code is copied.
 * Branch: lab/private-gsp615. Documented in docs/gsp615-candidate21.md.
 */
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/slab.h>

#include <subdev/gsp.h>
#include <rm/rm.h>

#include "pmgr_private.h"

#define INFO_POLICY_MASK   0x04U	/* u32 bit per valid policy */
#define INFO_IDX_PRIMARY   0x98U	/* u8 policy index, 0xff = none */
#define INFO_IDX_FALLBACK  0x9aU
#define INFO_IDX_FALLBACK2 0xb7U
#define INFO_POLICY_BASE   0xccU
#define INFO_POLICY_STRIDE 0x104U
#define POLICY_UNAVAILABLE 0x06U	/* u8, nonzero: NVML returns UNKNOWN */
#define POLICY_LIMIT_MIN   0x08U	/* u32 mW */
#define POLICY_LIMIT_RATED 0x0cU	/* u32 mW */
#define POLICY_LIMIT_MAX   0x10U	/* u32 mW */

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

int
r615_pmgr_parse_limits(const u8 *info, size_t bytes, struct r615_pmgr_limits *out)
{
	u32 mask, idx;
	const u8 *pol;

	if (!info || bytes < R615_PMGR_POLICY_INFO_BYTES)
		return -EINVAL;
	mask = rd32(info + INFO_POLICY_MASK);
	idx = info[INFO_IDX_PRIMARY];
	if (idx == 0xff)
		idx = info[INFO_IDX_FALLBACK];
	if (idx == 0xff)
		idx = info[INFO_IDX_FALLBACK2];
	if (idx >= 32 || !(mask & (1U << idx)))
		return -ENODATA;
	if (INFO_POLICY_BASE + (idx + 1) * INFO_POLICY_STRIDE > bytes)
		return -ENODATA;
	pol = info + INFO_POLICY_BASE + idx * INFO_POLICY_STRIDE;
	if (pol[POLICY_UNAVAILABLE])
		return -ENODATA;
	out->min_mw = rd32(pol + POLICY_LIMIT_MIN);
	out->rated_mw = rd32(pol + POLICY_LIMIT_RATED);
	out->max_mw = rd32(pol + POLICY_LIMIT_MAX);
	/* Sanity: non-zero, ordered, below 2 kW. Never report invented limits. */
	if (!out->min_mw || out->min_mw > out->max_mw || out->max_mw > 2000000 ||
	    out->rated_mw < out->min_mw || out->rated_mw > out->max_mw)
		return -ENODATA;
	return 0;
}

int
r615_pmgr_query_limits(struct nvkm_gsp *gsp, struct r615_pmgr_limits *out)
{
	struct nvkm_gsp_object *object;
	u8 *ctrl;
	int ret;

	if (!gsp || !gsp->running || !gsp->internal.device.subdevice.client)
		return -ENODEV;
	object = &gsp->internal.device.subdevice;
	ctrl = nvkm_gsp_rm_ctrl_get(object, R615_PMGR_POLICY_INFO_CTRL,
				    R615_PMGR_POLICY_INFO_BYTES);
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);
	/* Request is all zero, as in the reference. */
	memset(ctrl, 0, R615_PMGR_POLICY_INFO_BYTES);
	ret = nvkm_gsp_rm_ctrl_push(object, &ctrl, R615_PMGR_POLICY_INFO_BYTES);
	if (!ret)
		ret = r615_pmgr_parse_limits(ctrl, R615_PMGR_POLICY_INFO_BYTES, out);
	nvkm_gsp_rm_ctrl_done(object, ctrl);
	return ret;
}
