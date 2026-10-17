// SPDX-License-Identifier: MIT
/* Conservative bound independently derived from public 615 mem_mgr_pwr_mgmt:
 * include the entire gap above the last eligible user region, rather than
 * subtracting CPU-RM reserved memory or its BAR2 PTE exclusion. CPU instmem is
 * preserved separately. Unknown geometry fails, never shrinks the allocation.
 */
#include <linux/errno.h>
#include "pm.h"

int
r615_pm_plan(const struct r615_wpr_meta *wpr, const struct r615_static *info, u64 cbc_bytes,
		 struct r615_pm_plan *output)
{
	u64 last = 0, end, bytes;

	if (!wpr || !info || !output || !info->region_count ||
	    info->region_count > R615_STATIC_MAX_REGIONS || info->fb_length != wpr->fb_bytes ||
	    wpr->non_wpr_start > ~0ULL - wpr->non_wpr_bytes)
		return -EINVAL;
	for (u32 i = 0; i < info->region_count; i++) {
		const struct r615_fb_region *region = &info->regions[i];

		if (!region->flags_valid || !region->tag_valid)
			return -ENOENT;
		if (region->base > region->limit || region->limit >= info->fb_length ||
		    region->reserved > region->limit - region->base + 1 ||
		    region->tag > 5 || region->flags & ~7U)
			return -EPROTO;
		for (u32 j = 0; j < i; j++) {
			if (region->base <= info->regions[j].limit && info->regions[j].base <= region->limit)
				return -EPROTO;
		}
		if (r615_fb_region_usable(region) && region->limit + 1 > last)
			last = region->limit + 1;
	}
	end = wpr->non_wpr_start + wpr->non_wpr_bytes;
	if (!last || last > wpr->non_wpr_start || end != wpr->wpr_start ||
	    end > wpr->fb_bytes || !wpr->vga_bytes || wpr->vga_bytes > wpr->fb_bytes - end)
		return -EINVAL;
	bytes = end - last;
	if (bytes > ~0ULL - wpr->vga_bytes || bytes + wpr->vga_bytes > ~0ULL - cbc_bytes)
		return -EOVERFLOW;
	bytes += wpr->vga_bytes + cbc_bytes;
	if (bytes > ~0ULL - 4095)
		return -EOVERFLOW;
	bytes = (bytes + 4095) & ~4095ULL;
	if (!bytes || bytes / 4096 > 0xffff)
		return -E2BIG;
	*output = (struct r615_pm_plan) { .backup_bytes = bytes, .pages = bytes / 4096 };
	return 0;
}
