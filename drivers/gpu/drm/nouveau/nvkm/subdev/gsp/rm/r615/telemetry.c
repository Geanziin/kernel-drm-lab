// SPDX-License-Identifier: MIT
/* GA107 615.78.08 telemetry from the public RUSD (NV00DE) producer.
 *
 * GSP-RM writes the shared page; nouveau only reads it. Values are published
 * only after a validated, sequence-consistent snapshot whose timestamp has
 * been seen advancing recently. Missing or stale data stays an error.
 */
#include <linux/array_size.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <rm/rm.h>

#include "pmgr_private.h"
#include "rusd_session.h"
#include "telemetry.h"

#define R615_TELEMETRY_GROUPS 5

struct r615_telemetry {
	struct mutex mutex;
	struct r615_rusd_session *session;
	bool active;
	struct { u64 stamp; ktime_t seen; } group[R615_TELEMETRY_GROUPS];
	struct r615_pmgr_limits limits;
	int limits_status;
};

static int
r615_telemetry_group(enum r615_rusd_metric metric)
{
	switch (metric) {
	case R615_RUSD_GPU_BUSY:
	case R615_RUSD_MEMORY_BUSY:
		return 0;
	case R615_RUSD_ENCODER_BUSY:
	case R615_RUSD_DECODER_BUSY:
		return 1;
	case R615_RUSD_GPU_CLOCK_HZ:
	case R615_RUSD_MEMORY_CLOCK_HZ:
		return 2;
	case R615_RUSD_GPU_TEMP_MC:
	case R615_RUSD_MEMORY_TEMP_MC:
	case R615_RUSD_BOARD_TEMP_MC:
		return 3;
	case R615_RUSD_GPU_POWER_AVG_UW:
	case R615_RUSD_GPU_POWER_INST_UW:
	case R615_RUSD_GPU_POWER_LIMIT_REQUESTED_UW:
	case R615_RUSD_GPU_POWER_LIMIT_ENFORCED_UW:
		return 4;
	default:
		return -EINVAL;
	}
}

int
r615_telemetry_start(struct nvkm_gsp *gsp)
{
	struct r615_telemetry *tm = gsp->r615_telemetry;
	int ret;

	if (!tm) {
		tm = kzalloc(sizeof(*tm), GFP_KERNEL);
		if (!tm)
			return -ENOMEM;
		mutex_init(&tm->mutex);
		ret = r615_rusd_session_new(gsp, &tm->session);
		if (ret) {
			mutex_destroy(&tm->mutex);
			kfree(tm);
			return ret;
		}
		gsp->r615_telemetry = tm;
	}
	mutex_lock(&tm->mutex);
	ret = tm->active ? -EALREADY :
	      r615_rusd_session_start(tm->session, R615_TELEMETRY_POLL_MASK,
				      R615_TELEMETRY_INTERVAL_MS);
	if (!ret) {
		tm->active = true;
		memset(tm->group, 0, sizeof(tm->group));
	}
	mutex_unlock(&tm->mutex);
	if (!ret) {
		/* Static policy limits: query once (20 KiB RPC), cache, log once. */
		struct r615_pmgr_limits lim = {};
		int lret = r615_pmgr_query_limits(gsp, &lim);

		mutex_lock(&tm->mutex);
		tm->limits = lim;
		tm->limits_status = lret;
		mutex_unlock(&tm->mutex);
		if (lret)
			nvkm_info(&gsp->subdev, "r615: power policy limits unavailable (%d)\n", lret);
		else
			nvkm_info(&gsp->subdev, "r615: power policy limits min %u rated %u max %u mW\n",
				  lim.min_mw, lim.rated_mw, lim.max_mw);
	}
	return ret;
}

int
r615_telemetry_stop(struct nvkm_gsp *gsp)
{
	struct r615_telemetry *tm = gsp->r615_telemetry;
	int ret;

	if (!tm)
		return 0;
	mutex_lock(&tm->mutex);
	tm->active = false;
	ret = r615_rusd_session_stop(tm->session);
	mutex_unlock(&tm->mutex);
	return ret;
}

void
r615_telemetry_quiesced(struct nvkm_gsp *gsp)
{
	struct r615_telemetry *tm = gsp->r615_telemetry;

	if (!tm)
		return;
	mutex_lock(&tm->mutex);
	tm->active = false;
	r615_rusd_session_quiesced(tm->session);
	mutex_unlock(&tm->mutex);
}

void
r615_telemetry_del(struct nvkm_gsp *gsp)
{
	struct r615_telemetry *tm = gsp->r615_telemetry;

	if (!tm)
		return;
	/* A still-registered page (no proven stop) stays pinned: leak, not UAF. */
	if (r615_rusd_session_del(&tm->session) && tm->session) {
		nvkm_error(&gsp->subdev, "r615: telemetry page still registered, retained\n");
		return;
	}
	mutex_destroy(&tm->mutex);
	kfree(tm);
	gsp->r615_telemetry = NULL;
}

int
r615_telemetry_read(struct nvkm_gsp *gsp, enum r615_rusd_metric metric, s64 *value)
{
	struct r615_telemetry *tm = gsp ? gsp->r615_telemetry : NULL;
	struct r615_rusd_value sample;
	int group = r615_telemetry_group(metric);
	ktime_t now = ktime_get();
	int ret;

	if (!tm || group < 0 || !value)
		return tm ? -EINVAL : -ENODEV;
	mutex_lock(&tm->mutex);
	if (!tm->active) {
		ret = -ENODEV;
		goto done;
	}
	/* Clock-domain-neutral read: the reader's own age check is disabled
	 * and replaced by observed timestamp progress below.
	 */
	ret = r615_rusd_session_read(tm->session, metric, U64_MAX, U64_MAX, &sample);
	if (ret)
		goto done;
	if (sample.timestamp != tm->group[group].stamp) {
		tm->group[group].stamp = sample.timestamp;
		tm->group[group].seen = now;
	} else if (ktime_ms_delta(now, tm->group[group].seen) > R615_TELEMETRY_STALE_MS) {
		ret = -ESTALE;
		goto done;
	}
	*value = sample.value;
done:
	mutex_unlock(&tm->mutex);
	return ret;
}

/* Public ctrl2080perf.h NV2080_CTRL_CMD_PERF_GET_VID_ENG_PERFMON_SAMPLE. */
#define R615_PERF_VID_ENG_SAMPLE 0x20802087U
#define R615_PERF_VID_ENG_NVENC 1U
#define R615_PERF_VID_ENG_NVDEC 2U
struct r615_vid_eng_sample { u32 engine_type, clk_percent_busy, sampling_period_us; };

int
r615_telemetry_vid_eng(struct nvkm_gsp *gsp, bool encoder, s64 *percent)
{
	struct r615_telemetry *tm = gsp ? gsp->r615_telemetry : NULL;
	struct nvkm_gsp_object *object;
	struct r615_vid_eng_sample *ctrl;
	int ret;

	if (!tm)
		return -ENODEV;
	mutex_lock(&tm->mutex);
	if (!tm->active || !gsp->running || !gsp->internal.device.subdevice.client) {
		mutex_unlock(&tm->mutex);
		return -ENODEV;
	}
	object = &gsp->internal.device.subdevice;
	ctrl = nvkm_gsp_rm_ctrl_get(object, R615_PERF_VID_ENG_SAMPLE, sizeof(*ctrl));
	if (IS_ERR(ctrl)) {
		mutex_unlock(&tm->mutex);
		return PTR_ERR(ctrl);
	}
	ctrl->engine_type = encoder ? R615_PERF_VID_ENG_NVENC : R615_PERF_VID_ENG_NVDEC;
	ret = nvkm_gsp_rm_ctrl_push(object, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(object, ctrl);
		mutex_unlock(&tm->mutex);
		return ret;
	}
	/* The firmware reports a busy percentage over the sample window. A zero
	 * sampling period means no valid measurement, not 0%% utilization.
	 */
	if (!ctrl->sampling_period_us || ctrl->clk_percent_busy > 100)
		ret = -ENODATA;
	else
		*percent = ctrl->clk_percent_busy;
	nvkm_gsp_rm_ctrl_done(object, ctrl);
	mutex_unlock(&tm->mutex);
	return ret;
}

int
r615_telemetry_power_cap_max(struct nvkm_gsp *gsp, s64 *microwatt)
{
	struct r615_telemetry *tm = gsp ? gsp->r615_telemetry : NULL;
	int ret;

	if (!tm)
		return -ENODEV;
	mutex_lock(&tm->mutex);
	ret = !tm->active ? -ENODEV : tm->limits_status;
	if (!ret)
		*microwatt = (s64)tm->limits.max_mw * 1000;
	mutex_unlock(&tm->mutex);
	return ret;
}

static int
r615_telemetry_api_read(struct nvkm_gsp *gsp, enum nvkm_rm_telemetry which, s64 *value)
{
	static const enum r615_rusd_metric map[] = {
		[NVKM_RM_TM_GPU_BUSY_PCT] = R615_RUSD_GPU_BUSY,
		[NVKM_RM_TM_ENCODER_BUSY_PCT] = R615_RUSD_ENCODER_BUSY,
		[NVKM_RM_TM_DECODER_BUSY_PCT] = R615_RUSD_DECODER_BUSY,
		[NVKM_RM_TM_GPU_CLOCK_HZ] = R615_RUSD_GPU_CLOCK_HZ,
		[NVKM_RM_TM_MEMORY_CLOCK_HZ] = R615_RUSD_MEMORY_CLOCK_HZ,
		[NVKM_RM_TM_GPU_TEMP_MC] = R615_RUSD_GPU_TEMP_MC,
		[NVKM_RM_TM_POWER_AVG_UW] = R615_RUSD_GPU_POWER_AVG_UW,
		/* The enforced limit is the cap in force now, not the max settable. */
		[NVKM_RM_TM_POWER_CAP_UW] = R615_RUSD_GPU_POWER_LIMIT_ENFORCED_UW,
	};

	if (which == NVKM_RM_TM_POWER_CAP_MAX_UW)
		return r615_telemetry_power_cap_max(gsp, value);
	if (which == NVKM_RM_TM_ENCODER_BUSY_PCT)
		return r615_telemetry_vid_eng(gsp, true, value);
	if (which == NVKM_RM_TM_DECODER_BUSY_PCT)
		return r615_telemetry_vid_eng(gsp, false, value);
	if ((unsigned int)which >= ARRAY_SIZE(map))
		return -EINVAL;
	return r615_telemetry_read(gsp, map[which], value);
}

const struct nvkm_rm_api_telemetry r615_telemetry_api = {
	.read = r615_telemetry_api_read,
};
