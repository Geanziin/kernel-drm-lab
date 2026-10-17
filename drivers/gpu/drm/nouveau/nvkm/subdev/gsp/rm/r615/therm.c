/* SPDX-License-Identifier: MIT */
#include <rm/rm.h>

#include "therm.h"

static int
r615_therm_read(struct nvkm_gsp *gsp, u32 opcode, u32 index, u32 field, u32 *value)
{
	struct nvkm_gsp_object *object = &gsp->internal.device.subdevice;
	struct r615_therm_params *params;
	int ret;

	static_assert(sizeof(struct r615_therm_instruction) == 44);
	static_assert(sizeof(struct r615_therm_params) == 1432);

	if (!gsp->running || !object->client)
		return -EAGAIN;
	if (field >= ARRAY_SIZE(params->instructions[0].operands))
		return -EINVAL;

	params = nvkm_gsp_rm_ctrl_get(object, R615_THERM_EXECUTE_PHYSICAL, sizeof(*params));
	if (IS_ERR(params))
		return PTR_ERR(params);
	if (!params)
		return -EIO;

	memset(params, 0, sizeof(*params));
	params->version = 1;
	params->instruction_size = sizeof(struct r615_therm_instruction);
	params->count = 1;
	params->instructions[0].opcode = opcode;
	params->instructions[0].operands[0] = index;

	ret = nvkm_gsp_rm_ctrl_push(object, &params, sizeof(*params));
	if (!ret && !params)
		return -EIO;
	if (!ret && !r615_therm_reply_valid(params, opcode))
		ret = -EIO;
	if (!ret)
		*value = params->instructions[0].operands[field];
	if (params)
		nvkm_gsp_rm_ctrl_done(object, params);

	return ret;
}

static int
r615_therm_gpu_sensor(struct nvkm_gsp *gsp, u32 *sensor)
{
	u32 count, target, type, i;
	int ret;

	ret = r615_therm_read(gsp, R615_THERM_SENSOR_COUNT, 0, 0, &count);
	if (ret)
		return ret;
	/* Bound an untrusted firmware count in this opt-in GA107 prototype. */
	if (count > 32)
		return -E2BIG;

	for (i = 0; i < count; i++) {
		ret = r615_therm_read(gsp, R615_THERM_SENSOR_TARGET, i, 1, &target);
		if (ret)
			return ret;
		ret = r615_therm_read(gsp, R615_THERM_TARGET_TYPE, target, 1, &type);
		if (ret)
			return ret;
		if (type == R615_THERM_TARGET_GPU) {
			*sensor = i;
			return 0;
		}
	}

	return -ENODEV;
}

static int
r615_therm_temp_get(struct nvkm_gsp *gsp, u32 sensor, long *millidegrees)
{
	u32 reading;
	int ret;

	ret = r615_therm_read(gsp, R615_THERM_SENSOR_READING, sensor, 1, &reading);
	if (ret)
		return ret;
	return r615_therm_scale_temperature((s32)reading, millidegrees);
}

const struct nvkm_rm_api_therm
r615_therm = {
	.gpu_sensor = r615_therm_gpu_sensor,
	.temp_get = r615_therm_temp_get,
};
