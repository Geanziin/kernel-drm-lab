// SPDX-License-Identifier: MIT
#include <linux/build_bug.h>
#include <linux/errno.h>
#include "sr.h"

static_assert(sizeof(struct r615_sr_meta) == 256);

int
r615_sr_size(const struct r615_wpr_meta *wpr, u64 *bytes)
{
	u64 start, end;

	if (!wpr || !bytes || wpr->magic != R615_WPR_MAGIC || wpr->revision != R615_WPR_REVISION)
		return -EINVAL;
	if (wpr->vf_count || wpr->paging_config || (wpr->flags & ~1U))
		return -EOPNOTSUPP;
	if (!wpr->non_wpr_bytes || !wpr->frts_bytes ||
	    wpr->non_wpr_start > ~0ULL - wpr->non_wpr_bytes ||
	    wpr->frts_start > ~0ULL - wpr->frts_bytes)
		return -EINVAL;
	start = wpr->non_wpr_start + wpr->non_wpr_bytes;
	end = wpr->frts_start + wpr->frts_bytes;
	if (!start || start != wpr->wpr_start || end != wpr->wpr_end ||
	    start >= end || end > wpr->fb_bytes || start % R615_BOOT_PAGE || end % R615_BOOT_PAGE)
		return -EINVAL;
	if (end - start > R615_SR_MAX_BYTES)
		return -E2BIG;
	*bytes = end - start;
	return 0;
}

int
r615_sr_meta_build(const struct r615_wpr_meta *wpr, u64 radix, u64 bytes,
		   struct r615_sr_meta *output)
{
	struct r615_sr_meta meta = { 0 };
	u64 expected;
	int ret;

	if (!output || !radix || radix % R615_BOOT_PAGE || radix >= (1ULL << 49))
		return -EINVAL;
	ret = r615_sr_size(wpr, &expected);
	if (ret)
		return ret;
	if (bytes != expected)
		return -EINVAL;
	meta.magic = R615_SR_MAGIC;
	meta.revision = R615_SR_REVISION;
	meta.radix = radix;
	meta.bytes = bytes;
	meta.flags = wpr->flags & 1;
	*output = meta;
	return 0;
}
