// SPDX-License-Identifier: MIT
/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved. */

#include <linux/errno.h>
#include <linux/string.h>

#include "nvkv.h"
#include "sysinfo.h"

int
r615_sysinfo_encode(const struct r615_sysinfo *in, u64 *words,
		    u32 limit, u32 *count)
{
	struct r615_nvkv_enc enc;
	u32 pci[3];
	u64 phys[4];
	u64 flags[4];
	int ret;

	if (!in || !words || !count || limit > R615_NVKV_MAX_WORDS)
		return -EINVAL;
	if (!in->phys_bar0 || !in->phys_fb || !in->max_user_va)
		return -EINVAL;

	r615_nvkv_begin(&enc, words, limit);

	pci[0] = in->pci_device;
	pci[1] = in->pci_sub;
	pci[2] = in->pci_rev;
	ret = r615_nvkv_seq32(&enc, 0, R615_SI_PCI_DEVICE_ID, pci, 3);
	if (ret)
		return ret;

	phys[0] = in->phys_bar0;
	phys[1] = in->phys_fb;
	phys[2] = in->phys_inst;
	phys[3] = 0; /* IO addr not used on GA107 baremetal */
	ret = r615_nvkv_seq64(&enc, 0, R615_SI_GPU_PHYS_ADDR, phys, 4);
	if (ret)
		return ret;

	ret = r615_nvkv_seq64(&enc, 0, R615_SI_DOMAIN_BDF,
			      (u64[]){ in->domain_bdf }, 1);
	if (ret)
		return ret;

	ret = r615_nvkv_imm32(&enc, 0, R615_SI_HOST_ARCH,
			      R615_SI_HOST_ARCH_X86_64);
	if (ret)
		return ret;
	ret = r615_nvkv_imm32(&enc, 0, R615_SI_HOST_OS_TYPE,
			      R615_SI_HOST_OS_UNIX);
	if (ret)
		return ret;
	ret = r615_nvkv_imm32(&enc, 0, R615_SI_HOST_PAGE_SIZE, 4096);
	if (ret)
		return ret;

	ret = r615_nvkv_seq32(&enc, 0, R615_SI_PCI_MIRROR_BASE,
			      (u32[]){ in->cfg_base, in->cfg_size }, 2);
	if (ret)
		return ret;

	flags[0] = 0; /* PCIE_FLAGS minimal */
	flags[1] = in->primary ? R615_SI_PLATFORM_PRIMARY : 0;
	flags[2] = 0; /* FEATURE_FLAGS minimal */
	flags[3] = 0; /* VF_FLAGS */
	ret = r615_nvkv_seq64(&enc, 0, R615_SI_PCIE_FLAGS, flags, 4);
	if (ret)
		return ret;

	{
		u64 vals[3] = { 0, in->console_mem, in->max_user_va };

		ret = r615_nvkv_seq64(&enc, 0, R615_SI_ATOMICS_OP_MASK,
				      vals, 3);
		if (ret)
			return ret;
	}

	ret = r615_nvkv_arr8(&enc, 0, R615_SI_DRIVER_NAME,
			     "nouveau", sizeof("nouveau"));
	if (ret)
		return ret;
	ret = r615_nvkv_arr8(&enc, 0, R615_SI_DRIVER_VERSION,
			     "615.78.08", sizeof("615.78.08"));
	if (ret)
		return ret;
	ret = r615_nvkv_imm32(&enc, 0, R615_SI_DRIVER_CHANGELIST, 0);
	if (ret)
		return ret;
	ret = r615_nvkv_imm32(&enc, 0, R615_SI_UNSTABLE,
			      R615_SI_UNSTABLE_RPC | R615_SI_UNSTABLE_EVENTS);
	if (ret)
		return ret;

	*count = enc.count;
	return 0;
}

static bool
r615_static_known(u32 key)
{
	return (key >= R615_SC_INTERNAL_CLIENT && key <= R615_SC_INTERNAL_SUBDEVICE) ||
	       key == R615_SC_FB_REGION_COUNT ||
	       key == R615_SC_FB_REGION_FLAGS || key == R615_SC_FB_REGION_TAG ||
	       (key >= R615_SC_FB_LENGTH && key <= R615_SC_FB_REGION_RESERVED) ||
	       key == R615_SC_BAR1_PDB || key == R615_SC_BAR2_PDB;
}

static int
r615_static_value(struct r615_static *res, u32 key, u32 idx, u64 value,
		   bool wide, u32 *seen, u8 region_seen[R615_STATIC_MAX_REGIONS])
{
	u32 bit;
	bool wide_region = key >= R615_SC_FB_REGION_BASE && key <= R615_SC_FB_REGION_RESERVED;
	bool region = wide_region || key == R615_SC_FB_REGION_FLAGS || key == R615_SC_FB_REGION_TAG;
	bool expected_wide = wide_region || key == R615_SC_FB_LENGTH ||
			     key == R615_SC_BAR1_PDB || key == R615_SC_BAR2_PDB;

	if (!r615_static_known(key))
		return 0;
	if (wide != expected_wide || (region ? idx >= R615_STATIC_MAX_REGIONS : idx != 0))
		return -EPROTO;
	if (region) {
		bit = wide_region ? 1U << (key - R615_SC_FB_REGION_BASE) :
		      key == R615_SC_FB_REGION_FLAGS ? 8 : 16;
		if (region_seen[idx] & bit)
			return -EPROTO;
		region_seen[idx] |= bit;
		if (key == R615_SC_FB_REGION_BASE)
			res->regions[idx].base = value;
		else if (key == R615_SC_FB_REGION_LIMIT)
			res->regions[idx].limit = value;
		else if (key == R615_SC_FB_REGION_RESERVED)
			res->regions[idx].reserved = value;
		else if (key == R615_SC_FB_REGION_FLAGS) {
			if (value & ~7ULL)
				return -EPROTO;
			res->regions[idx].flags = value;
			res->regions[idx].flags_valid = true;
		} else {
			if (value > 5)
				return -EOPNOTSUPP;
			res->regions[idx].tag = value;
			res->regions[idx].tag_valid = true;
		}
		return 0;
	}
	if (key >= R615_SC_INTERNAL_CLIENT && key <= R615_SC_INTERNAL_SUBDEVICE) {
		bit = 1U << (key - R615_SC_INTERNAL_CLIENT);
		if (!value)
			return -EPROTO;
		if (key == R615_SC_INTERNAL_CLIENT)
			res->client = value;
		else if (key == R615_SC_INTERNAL_DEVICE)
			res->device = value;
		else
			res->subdevice = value;
	} else if (key == R615_SC_FB_REGION_COUNT) {
		bit = 1U << 3;
		if (!value || value > R615_STATIC_MAX_REGIONS)
			return -EPROTO;
		res->region_count = value;
	} else if (key == R615_SC_BAR1_PDB) {
		bit = 1U << 4;
		res->bar1_pdb = value;
	} else if (key == R615_SC_BAR2_PDB) {
		bit = 1U << 5;
		res->bar2_pdb = value;
	} else {
		bit = 1U << 6;
		if (!value)
			return -EPROTO;
		res->fb_length = value;
	}
	if (*seen & bit)
		return -EPROTO;
	*seen |= bit;
	return 0;
}

int
r615_static_decode(const u64 *words, u32 count, struct r615_static *out)
{
	struct r615_nvkv_dec dec = { .words = words, .count = count };
	struct r615_static res = { 0 };
	u8 region_seen[R615_STATIC_MAX_REGIONS] = { 0 };
	u32 seen = 0;

	if (!words || !out || !count || count > R615_NVKV_MAX_WORDS)
		return -EINVAL;
	while (dec.pos < dec.count) {
		u32 key, idx, op, n;
		u64 bytes, payload_words;
		bool wide;
		int ret = r615_nvkv_next(&dec, &key, &idx, &op, &n);

		if (ret)
			return ret;
		if (op == R615_NVKV_OP_IMM32) {
			ret = r615_static_value(&res, key, idx, n, false, &seen, region_seen);
			if (ret)
				return ret;
			continue;
		}
		wide = op == R615_NVKV_OP_SEQ64 || op == R615_NVKV_OP_ARR64;
		bytes = (u64)n * (op == R615_NVKV_OP_ARR8 ? 1 : wide ? 8 : 4);
		payload_words = (bytes + 7) / 8;
		if (payload_words > dec.count - dec.pos)
			return -EPROTO;
		if (op == R615_NVKV_OP_SEQ32 || op == R615_NVKV_OP_SEQ64) {
			/* SEQ increments the KEY, not the region index. */
			if (!n || n > 0x10000U - key)
				return -EPROTO;
			for (u32 i = 0; i < n; i++) {
				u64 value = 0;

				memcpy(&value, (const u8 *)&words[dec.pos] + (u64)i * (wide ? 8 : 4),
				       wide ? 8 : 4);
				ret = r615_static_value(&res, key + i, idx, value, wide, &seen, region_seen);
				if (ret)
					return ret;
			}
		} else if (r615_static_known(key)) {
			u64 value = 0;

			/* ARRAY is one key with a value vector. Required scalar keys
			 * may only use a single correctly typed value, as in 615.
			 */
			if (op == R615_NVKV_OP_ARR8 || n != 1)
				return -EPROTO;
			memcpy(&value, &words[dec.pos], wide ? 8 : 4);
			ret = r615_static_value(&res, key, idx, value, wide, &seen, region_seen);
			if (ret)
				return ret;
		}
		/* Unused strings/arrays and indexed keys are bounded and skipped. */
		dec.pos += payload_words;
	}
	if (seen != 0x7f)
		return -ENOENT;
	for (u32 i = 0; i < R615_STATIC_MAX_REGIONS; i++) {
		if (i >= res.region_count) {
			if (region_seen[i])
				return -EPROTO;
		} else if ((region_seen[i] & 7) != 7 || res.regions[i].limit < res.regions[i].base ||
			   res.regions[i].limit >= res.fb_length ||
			   res.regions[i].reserved > res.regions[i].limit - res.regions[i].base + 1) {
			return -EPROTO;
		}
		for (u32 j = 0; j < i && i < res.region_count; j++) {
			if (res.regions[i].base <= res.regions[j].limit &&
			    res.regions[j].base <= res.regions[i].limit)
				return -EPROTO;
		}
	}
	*out = res;
	return 0;
}
