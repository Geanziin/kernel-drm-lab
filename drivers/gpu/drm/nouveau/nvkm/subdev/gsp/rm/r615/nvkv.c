// SPDX-License-Identifier: MIT
/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved. */

#include <linux/errno.h>
#include <linux/string.h>

#include "nvkv.h"

void
r615_nvkv_begin(struct r615_nvkv_enc *enc, u64 *words, u32 limit)
{
	enc->words = words;
	enc->count = 0;
	enc->limit = words ? limit : 0;
}

static int
r615_nvkv_put(struct r615_nvkv_enc *enc, u64 word)
{
	if (enc->count >= enc->limit)
		return -ENOSPC;
	enc->words[enc->count++] = word;
	return 0;
}

int
r615_nvkv_imm32(struct r615_nvkv_enc *enc, u32 idx, u32 key, u32 value)
{
	u64 instr;

	if (!enc || idx > 0xfff || key > 0xffff)
		return -EINVAL;
	instr = (u64)key | ((u64)(idx & 0xfff) << 16) |
		((u64)R615_NVKV_OP_IMM32 << 28) | ((u64)value << 32);
	return r615_nvkv_put(enc, instr);
}

int
r615_nvkv_seq32(struct r615_nvkv_enc *enc, u32 idx, u32 key,
		const u32 *values, u32 count)
{
	u64 instr;
	u32 words, i;
	int ret;

	if (!enc || !values || !count || idx > 0xfff || key > 0xffff ||
	    key + count - 1 > 0xffff)
		return -EINVAL;
	words = (count + 1) / 2;
	instr = (u64)key | ((u64)(idx & 0xfff) << 16) |
		((u64)R615_NVKV_OP_SEQ32 << 28) | ((u64)count << 32);
	ret = r615_nvkv_put(enc, instr);
	if (ret)
		return ret;
	for (i = 0; i < words; i++) {
		u64 word = values[2 * i];
		u32 hi = (2 * i + 1 < count) ? values[2 * i + 1] : 0;

		word |= (u64)hi << 32;
		ret = r615_nvkv_put(enc, word);
		if (ret)
			return ret;
	}
	return 0;
}

int
r615_nvkv_seq64(struct r615_nvkv_enc *enc, u32 idx, u32 key,
		const u64 *values, u32 count)
{
	u64 instr;
	int ret;

	if (!enc || !values || !count || idx > 0xfff || key > 0xffff ||
	    key + count - 1 > 0xffff)
		return -EINVAL;
	instr = (u64)key | ((u64)(idx & 0xfff) << 16) |
		((u64)R615_NVKV_OP_SEQ64 << 28) | ((u64)count << 32);
	ret = r615_nvkv_put(enc, instr);
	if (ret)
		return ret;
	for (u32 i = 0; i < count; i++) {
		ret = r615_nvkv_put(enc, values[i]);
		if (ret)
			return ret;
	}
	return 0;
}

int
r615_nvkv_arr8(struct r615_nvkv_enc *enc, u32 idx, u32 key,
	       const void *data, u32 bytes)
{
	u64 instr;
	u32 words, i;
	int ret;

	if (!enc || (!data && bytes) || idx > 0xfff || key > 0xffff)
		return -EINVAL;
	/* Always NUL-terminate like public nvkvSetString8: bytes includes NUL. */
	words = (bytes + 7) / 8;
	instr = (u64)key | ((u64)(idx & 0xfff) << 16) |
		((u64)R615_NVKV_OP_ARR8 << 28) | ((u64)bytes << 32);
	ret = r615_nvkv_put(enc, instr);
	if (ret)
		return ret;
	for (i = 0; i < words; i++) {
		u64 word = 0;
		u32 chunk = bytes - i * 8 > 8 ? 8 : bytes - i * 8;

		if (chunk)
			memcpy(&word, (const u8 *)data + i * 8, chunk);
		ret = r615_nvkv_put(enc, word);
		if (ret)
			return ret;
	}
	return 0;
}

int
r615_nvkv_next(struct r615_nvkv_dec *dec, u32 *key, u32 *idx,
	       u32 *op, u32 *count)
{
	u64 instr;

	if (!dec || !dec->words || !key || !idx || !op || !count)
		return -EINVAL;
	if (dec->pos >= dec->count)
		return -ENODATA;
	instr = dec->words[dec->pos++];
	*key = instr & 0xffff;
	*idx = (instr >> 16) & 0xfff;
	*op = (instr >> 28) & 0xf;
	*count = instr >> 32;
	if (*op > R615_NVKV_OP_ARR64)
		return -EPROTO;
	return 0;
}

int
r615_nvkv_take(struct r615_nvkv_dec *dec, void *out, u32 bytes)
{
	u64 words = ((u64)bytes + 7) / 8;

	if (!dec || !dec->words || (!out && bytes))
		return -EINVAL;
	if (dec->pos > dec->count || words > dec->count - dec->pos)
		return -EPROTO;
	if (bytes)
		memcpy(out, &dec->words[dec->pos], bytes);
	dec->pos += words;
	return 0;
}
