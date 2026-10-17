// SPDX-License-Identifier: MIT
/* GSP-RM telemetry exposure for GSP-booted GPUs (first user: GA107/615).
 *
 * hwmon "nouveau" (labelled channels; units per Documentation/hwmon):
 *   temp1  gpu     millidegree C
 *   power1 gpu     average + current enforced cap, microwatt
 *   freq1  gpu     Hz
 *   freq2  memory  Hz
 * Device sysfs: gpu_busy_percent, encoder_busy_percent, decoder_busy_percent.
 *
 * Every read goes to the producer; a missing or stale sample returns an
 * error to userspace, never a cached or invented value.
 */
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/sysfs.h>

#include <subdev/gsp.h>
#include <rm/rm.h>

#include "nouveau_drv.h"
#include "nouveau_gsp_telemetry.h"

static const struct nvkm_rm_api_telemetry *
nouveau_gsp_tm_api(struct nouveau_drm *drm, struct nvkm_gsp **pgsp)
{
	struct nvkm_gsp *gsp = nvxx_device(drm)->gsp;

	if (!nvkm_gsp_rm(gsp) || !gsp->rm || !gsp->rm->api || !gsp->rm->api->telemetry)
		return NULL;
	*pgsp = gsp;
	return gsp->rm->api->telemetry;
}

static int
nouveau_gsp_tm_get(struct nouveau_drm *drm, enum nvkm_rm_telemetry which, s64 *value)
{
	const struct nvkm_rm_api_telemetry *api;
	struct nvkm_gsp *gsp;

	api = nouveau_gsp_tm_api(drm, &gsp);
	if (!api)
		return -ENODEV;
	return api->read(gsp, which, value);
}

/* ---- hwmon ---- */

static const struct hwmon_channel_info * const nouveau_gsp_tm_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(power, HWMON_P_AVERAGE | HWMON_P_CAP | HWMON_P_CAP_MAX | HWMON_P_LABEL),
	NULL
};

static umode_t
nouveau_gsp_tm_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr, int ch)
{
	switch (type) {
	case hwmon_temp:
		return attr == hwmon_temp_input || attr == hwmon_temp_label ? 0444 : 0;
	case hwmon_power:
		return attr == hwmon_power_average || attr == hwmon_power_cap ||
		       attr == hwmon_power_cap_max || attr == hwmon_power_label ? 0444 : 0;
	default:
		return 0;
	}
}

static int
nouveau_gsp_tm_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int ch, long *val)
{
	struct nouveau_drm *drm = nouveau_drm(dev_get_drvdata(dev));
	enum nvkm_rm_telemetry which;
	s64 v;
	int ret;

	switch (type) {
	case hwmon_temp:
		which = NVKM_RM_TM_GPU_TEMP_MC;
		break;
	case hwmon_power:
		which = attr == hwmon_power_cap_max ? NVKM_RM_TM_POWER_CAP_MAX_UW :
			attr == hwmon_power_cap ? NVKM_RM_TM_POWER_CAP_UW : NVKM_RM_TM_POWER_AVG_UW;
		break;
	default:
		return -EOPNOTSUPP;
	}
	ret = nouveau_gsp_tm_get(drm, which, &v);
	if (ret)
		return ret;
	*val = v;
	return 0;
}

static int
nouveau_gsp_tm_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			   int ch, const char **str)
{
	switch (type) {
	case hwmon_temp:
	case hwmon_power:
		*str = "GPU";
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* No hwmon_freq channel type in this kernel: freq*_input (Hz) / freq*_label
 * as extra attributes, as amdgpu does. Owned by the hwmon device.
 */
static ssize_t
nouveau_gsp_tm_freq(struct device *dev, char *buf, enum nvkm_rm_telemetry which)
{
	struct nouveau_drm *drm = nouveau_drm(dev_get_drvdata(dev));
	s64 v;
	int ret = nouveau_gsp_tm_get(drm, which, &v);

	return ret ? ret : sysfs_emit(buf, "%lld\n", (long long)v);
}
static ssize_t freq1_input_show(struct device *d, struct device_attribute *a, char *b)
{ return nouveau_gsp_tm_freq(d, b, NVKM_RM_TM_GPU_CLOCK_HZ); }
static ssize_t freq2_input_show(struct device *d, struct device_attribute *a, char *b)
{ return nouveau_gsp_tm_freq(d, b, NVKM_RM_TM_MEMORY_CLOCK_HZ); }
static ssize_t freq1_label_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "GPU\n"); }
static ssize_t freq2_label_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "Memory\n"); }
static DEVICE_ATTR_RO(freq1_input);
static DEVICE_ATTR_RO(freq2_input);
static DEVICE_ATTR_RO(freq1_label);
static DEVICE_ATTR_RO(freq2_label);
static struct attribute *nouveau_gsp_tm_freq_attrs[] = {
	&dev_attr_freq1_input.attr, &dev_attr_freq1_label.attr,
	&dev_attr_freq2_input.attr, &dev_attr_freq2_label.attr,
	NULL
};
static const struct attribute_group nouveau_gsp_tm_freq_group = {
	.attrs = nouveau_gsp_tm_freq_attrs,
};
static const struct attribute_group *nouveau_gsp_tm_extra[] = {
	&nouveau_gsp_tm_freq_group, NULL
};

static const struct hwmon_ops nouveau_gsp_tm_ops = {
	.is_visible = nouveau_gsp_tm_is_visible,
	.read = nouveau_gsp_tm_read,
	.read_string = nouveau_gsp_tm_read_string,
};

static const struct hwmon_chip_info nouveau_gsp_tm_chip = {
	.ops = &nouveau_gsp_tm_ops,
	.info = nouveau_gsp_tm_info,
};

/* ---- device sysfs busy percentages ---- */

static ssize_t
nouveau_gsp_tm_busy(struct device *dev, char *buf, enum nvkm_rm_telemetry which)
{
	/* PCI device drvdata is the nouveau_drm (see nouveau_drm.c). */
	struct nouveau_drm *drm = dev_get_drvdata(dev);
	s64 v;
	int ret = nouveau_gsp_tm_get(drm, which, &v);

	return ret ? ret : sysfs_emit(buf, "%lld\n", (long long)v);
}

static ssize_t gpu_busy_percent_show(struct device *d, struct device_attribute *a, char *b)
{ return nouveau_gsp_tm_busy(d, b, NVKM_RM_TM_GPU_BUSY_PCT); }
/* Video engines: driver-side job accounting (GSP perfmon/RUSD report 0 for
 * nouveau-submitted work).  See nouveau_engine_busy.h for semantics.
 */
static ssize_t
nouveau_video_busy(struct device *dev, char *buf, enum nouveau_busy_engine e)
{
	struct nouveau_drm *drm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", nouveau_engine_busy_percent(&drm->video_busy[e]));
}
static ssize_t encoder_busy_percent_show(struct device *d, struct device_attribute *a, char *b)
{ return nouveau_video_busy(d, b, NOUVEAU_BUSY_NVENC); }
static ssize_t decoder_busy_percent_show(struct device *d, struct device_attribute *a, char *b)
{ return nouveau_video_busy(d, b, NOUVEAU_BUSY_NVDEC); }
static DEVICE_ATTR_RO(gpu_busy_percent);
static DEVICE_ATTR_RO(encoder_busy_percent);
static DEVICE_ATTR_RO(decoder_busy_percent);

static struct attribute *nouveau_gsp_tm_attrs[] = {
	&dev_attr_gpu_busy_percent.attr,
	&dev_attr_encoder_busy_percent.attr,
	&dev_attr_decoder_busy_percent.attr,
	NULL
};
static const struct attribute_group nouveau_gsp_tm_group = {
	.attrs = nouveau_gsp_tm_attrs,
};

int
nouveau_gsp_telemetry_init(struct drm_device *dev)
{
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct device *hwmon;
	struct nvkm_gsp *gsp;
	int ret;

	if (!nouveau_gsp_tm_api(drm, &gsp))
		return 0;
	/* The group is the sysfs owner; remove it before teardown drains readers. */
	ret = sysfs_create_group(&dev->dev->kobj, &nouveau_gsp_tm_group);
	if (ret)
		return ret;
	drm->gsp_tm_group = true;
	hwmon = hwmon_device_register_with_info(dev->dev, "nouveau", dev,
						&nouveau_gsp_tm_chip, nouveau_gsp_tm_extra);
	if (IS_ERR(hwmon)) {
		NV_ERROR(drm, "GSP telemetry hwmon: %ld\n", PTR_ERR(hwmon));
		return PTR_ERR(hwmon);
	}
	drm->gsp_tm_hwmon = hwmon;
	return 0;
}

void
nouveau_gsp_telemetry_fini(struct drm_device *dev)
{
	struct nouveau_drm *drm = nouveau_drm(dev);

	if (drm->gsp_tm_hwmon) {
		hwmon_device_unregister(drm->gsp_tm_hwmon);
		drm->gsp_tm_hwmon = NULL;
	}
	if (drm->gsp_tm_group) {
		sysfs_remove_group(&dev->dev->kobj, &nouveau_gsp_tm_group);
		drm->gsp_tm_group = false;
	}
}
