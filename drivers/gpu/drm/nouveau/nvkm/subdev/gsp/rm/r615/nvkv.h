/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Minimal NVKV encoder/decoder for 615 GMCAPI (CPU + kernel).
 * Format per public src/nvidia/inc/libraries/nvkv/nvkv.h (MIT, 615.78.08):
 * u64 instr = key[15:0] | idx[27:16] | opcode[31:28] | count/imm[63:32],
 * followed by values, 8-byte aligned, zero-padded.
 * Independent implementation; no closed code.
 */
#ifndef __NVKM_R615_NVKV_H__
#define __NVKM_R615_NVKV_H__

#include <linux/types.h>

#define R615_NVKV_OP_IMM32  0x0u
#define R615_NVKV_OP_SEQ32  0x1u
#define R615_NVKV_OP_SEQ64  0x2u
#define R615_NVKV_OP_ARR8   0x3u
#define R615_NVKV_OP_ARR32  0x4u
#define R615_NVKV_OP_ARR64  0x5u

#define R615_NVKV_MAX_WORDS 8192u

struct r615_nvkv_enc {
	u64 *words;
	u32 count, limit;
};

void r615_nvkv_begin(struct r615_nvkv_enc *enc, u64 *words, u32 limit);
int r615_nvkv_imm32(struct r615_nvkv_enc *enc, u32 idx, u32 key, u32 value);
int r615_nvkv_seq32(struct r615_nvkv_enc *enc, u32 idx, u32 key,
		    const u32 *values, u32 count);
int r615_nvkv_seq64(struct r615_nvkv_enc *enc, u32 idx, u32 key,
		    const u64 *values, u32 count);
int r615_nvkv_arr8(struct r615_nvkv_enc *enc, u32 idx, u32 key,
		   const void *data, u32 bytes);

struct r615_nvkv_dec {
	const u64 *words;
	u32 count, pos;
};

/* Returns 0 with key/idx/op/count filled, advances past instr word only.
 * Caller must then validate and consume value words via r615_nvkv_take().
 */
int r615_nvkv_next(struct r615_nvkv_dec *dec, u32 *key, u32 *idx,
		   u32 *op, u32 *count);
int r615_nvkv_take(struct r615_nvkv_dec *dec, void *out, u32 bytes);

#endif
