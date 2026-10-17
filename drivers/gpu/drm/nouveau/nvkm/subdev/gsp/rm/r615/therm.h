/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_THERM_H__
#define __NVKM_R615_THERM_H__

#define R615_THERM_EXECUTE_PHYSICAL 0x20808513
#define R615_THERM_TARGET_TYPE      0x00000101
#define R615_THERM_SENSOR_COUNT     0x00000500
#define R615_THERM_SENSOR_TARGET    0x00000520
#define R615_THERM_SENSOR_READING   0x00001500
#define R615_THERM_TARGET_GPU       0x00000001
#define R615_THERM_MAX_INSTRUCTIONS 32

struct r615_therm_instruction {
	u32 result;
	u32 executed;
	u32 opcode;
	u32 operands[8];
};

struct r615_therm_params {
	u32 version;
	u32 revision;
	u32 instruction_size;
	u32 flags;
	u32 successful;
	u32 count;
	struct r615_therm_instruction instructions[R615_THERM_MAX_INSTRUCTIONS];
};

static inline bool
r615_therm_reply_valid(const struct r615_therm_params *params, u32 opcode)
{
	return params->version == 1 && params->revision == 0 &&
	       params->instruction_size == sizeof(struct r615_therm_instruction) &&
	       params->count == 1 && params->successful == 1 &&
	       params->instructions[0].executed == 1 &&
	       params->instructions[0].result == 0 &&
	       params->instructions[0].opcode == opcode;
}

static inline int
r615_therm_scale_temperature(s32 celsius, long *millidegrees)
{
	/* These bounds reject invalid/sentinel data before multiplying. */
	if (celsius < -273 || celsius > 1000)
		return -ERANGE;
	*millidegrees = (long)celsius * 1000;
	return 0;
}

#endif
