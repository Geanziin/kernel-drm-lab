// SPDX-License-Identifier: MIT
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "bootmem.h"

#define R615_MIB 0x100000ULL
#define R615_PRESCRUB_BYTES (256ULL << 20)

static_assert(sizeof(struct r615_wpr_meta) == 256);
static_assert(sizeof(struct r615_libos_region) == 32);
static bool
r615_bootmem_overlap(const void *left, size_t left_bytes, const void *right, size_t right_bytes)
{
	unsigned long a = (unsigned long)left, b = (unsigned long)right;

	if (a > ~0UL - left_bytes || b > ~0UL - right_bytes)
		return true;
	return left_bytes && right_bytes && a < b + right_bytes && b < a + left_bytes;
}

static bool
r615_bootmem_dma(u64 address, u64 bytes)
{
	return address && !(address % R615_BOOT_PAGE) && bytes && address <= ~0ULL - bytes;
}

int
r615_radix3_plan(u64 data_bytes, struct r615_radix3_plan *output)
{
	struct r615_radix3_plan plan = { .data_bytes = data_bytes };
	u64 pages;

	if (!output || !data_bytes)
		return -EINVAL;
	/* Three levels of 512 entries cover at most 512 GiB. */
	if (data_bytes > (1ULL << 39))
		return -E2BIG;
	pages = (data_bytes + R615_BOOT_PAGE - 1) / R615_BOOT_PAGE;
	plan.data_pages = pages;
	plan.leaf_pages = (pages + R615_RADIX_ENTRIES - 1) / R615_RADIX_ENTRIES;
	plan.directory_pages = (plan.leaf_pages + R615_RADIX_ENTRIES - 1) / R615_RADIX_ENTRIES;
	plan.table_bytes = (1U + plan.directory_pages + plan.leaf_pages) * R615_BOOT_PAGE;
	*output = plan;
	return 0;
}

int
r615_radix3_build(const struct r615_radix3_plan *input, void *tables, size_t bytes,
		  u64 table_address, const u64 *data_addresses, u32 data_pages)
{
	struct r615_radix3_plan plan;
	u64 leaf_offset;
	int ret;

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	return -EOPNOTSUPP;
#endif
	if (!input || !tables || !data_addresses)
		return -EINVAL;
	ret = r615_radix3_plan(input->data_bytes, &plan);
	if (ret)
		return ret;
	if (input->data_pages != plan.data_pages ||
	    input->directory_pages != plan.directory_pages ||
	    input->leaf_pages != plan.leaf_pages || input->table_bytes != plan.table_bytes ||
	    data_pages != plan.data_pages || bytes != plan.table_bytes ||
	    !r615_bootmem_dma(table_address, bytes) ||
	    r615_bootmem_overlap(tables, bytes, data_addresses, (size_t)data_pages * sizeof(u64)) ||
	    r615_bootmem_overlap(tables, bytes, input, sizeof(*input)))
		return -EINVAL;
	for (u32 i = 0; i < data_pages; i++) {
		if (!r615_bootmem_dma(data_addresses[i], R615_BOOT_PAGE) ||
		    (data_addresses[i] < table_address + bytes &&
		     table_address < data_addresses[i] + R615_BOOT_PAGE))
			return -EINVAL;
	}
	memset(tables, 0, bytes);
	/* Root -> directory pages -> leaf pages -> payload pages. No PTE flags. */
	for (u32 i = 0; i < plan.directory_pages; i++) {
		u64 address = table_address + (1ULL + i) * R615_BOOT_PAGE;

		memcpy((u8 *)tables + i * sizeof(address), &address, sizeof(address));
	}
	leaf_offset = (1ULL + plan.directory_pages) * R615_BOOT_PAGE;
	for (u32 i = 0; i < plan.leaf_pages; i++) {
		u64 address = table_address + leaf_offset + (u64)i * R615_BOOT_PAGE;

		memcpy((u8 *)tables + R615_BOOT_PAGE + i * sizeof(address),
		       &address, sizeof(address));
	}
	memcpy((u8 *)tables + leaf_offset, data_addresses, (size_t)data_pages * sizeof(u64));
	return 0;
}

/* Subtract first, then align down, without unsigned underflow. */
static bool
r615_wpr_below(u64 top, u64 bytes, u64 alignment, u64 *start)
{
	if (bytes > top)
		return false;
	*start = (top - bytes) & ~(alignment - 1);
	return true;
}

int
r615_wpr_build(const char *version, const struct r615_wpr_config *input,
	       struct r615_wpr_meta *output)
{
	struct r615_wpr_meta meta = { .magic = R615_WPR_MAGIC, .revision = R615_WPR_REVISION };
	struct r615_wpr_config config;
	u64 top, post, wanted_heap, max_heap;
	u64 addresses[4], sizes[4];

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	return -EOPNOTSUPP;
#endif
	if (!input || !output)
		return -EINVAL;
	if (!version || strcmp(version, "615.78.08") || input->chipset != 0x177)
		return -EOPNOTSUPP;
	config = *input;
	if (config.fb_bytes < R615_PRESCRUB_BYTES || config.fb_bytes > (1ULL << 40) ||
	    config.fb_bytes % R615_MIB || !config.elf_bytes || !config.boot_bytes ||
	    config.boot_code >= config.boot_bytes || config.boot_data >= config.boot_bytes ||
	    config.boot_manifest >= config.boot_bytes ||
	    !r615_bootmem_dma(config.elf_radix, R615_BOOT_PAGE) ||
	    !r615_bootmem_dma(config.boot_address, config.boot_bytes) ||
	    config.signature_bytes != R615_BOOT_PAGE ||
	    !r615_bootmem_dma(config.signature_address, config.signature_bytes) ||
	    (!!config.crash_address != !!config.crash_bytes) ||
	    (config.crash_bytes && !r615_bootmem_dma(config.crash_address, config.crash_bytes)))
		return -EINVAL;
	addresses[0] = config.elf_radix;
	addresses[1] = config.boot_address;
	addresses[2] = config.signature_address;
	addresses[3] = config.crash_address;
	sizes[0] = R615_BOOT_PAGE;
	sizes[1] = config.boot_bytes;
	sizes[2] = config.signature_bytes;
	sizes[3] = config.crash_bytes;
	for (u32 i = 0; i < 4; i++) {
		for (u32 j = 0; j < i; j++) {
			if (sizes[i] && addresses[i] < addresses[j] + sizes[j] &&
			    addresses[j] < addresses[i] + sizes[i])
				return -EINVAL;
		}
	}
	meta.fb_bytes = config.fb_bytes;
	meta.vga_start = meta.fb_bytes - R615_MIB;
	if (config.vga_valid) {
		if (config.vga_start >= meta.fb_bytes || config.vga_start % 0x10000)
			return -EINVAL;
		meta.vga_start = config.vga_start < meta.vga_start ?
				 meta.fb_bytes - 0x20000 : config.vga_start;
	}
	meta.vga_bytes = meta.fb_bytes - meta.vga_start;
	top = meta.vga_start;
	if (config.mmu_lock_valid) {
		if (!config.mmu_lock_start || config.mmu_lock_start > meta.fb_bytes ||
		    config.mmu_lock_start % R615_BOOT_PAGE)
			return -EINVAL;
		if (config.mmu_lock_start < top)
			top = config.mmu_lock_start;
	}
	meta.wpr_end = top & ~0x1ffffULL;
	meta.frts_bytes = R615_MIB;
	meta.elf_bytes = config.elf_bytes;
	meta.boot_bytes = config.boot_bytes;
	if (!r615_wpr_below(meta.wpr_end, meta.frts_bytes, R615_BOOT_PAGE, &meta.frts_start) ||
	    !r615_wpr_below(meta.frts_start, meta.boot_bytes, R615_BOOT_PAGE, &meta.boot_start) ||
	    !r615_wpr_below(meta.boot_start, meta.elf_bytes, 0x10000, &meta.elf_start))
		return -ENOSPC;
	post = (meta.fb_bytes - meta.elf_start + R615_MIB - 1) & ~(R615_MIB - 1);
	/* Non-WPR heap and the Booter metadata each reserve one MiB on GA107. */
	if (post + 2 * R615_MIB >= R615_PRESCRUB_BYTES)
		return -ENOSPC;
	max_heap = R615_PRESCRUB_BYTES - post - 2 * R615_MIB;
	/* Public 615 baremetal policy: OS 22 MiB + RM 8 MiB + clients 96 MiB. */
	wanted_heap = (126 * R615_MIB) +
		     ((((meta.fb_bytes + (1ULL << 30) - 1) >> 30) * (96 << 10) + R615_MIB - 1) &
		      ~(R615_MIB - 1));
	if (wanted_heap > max_heap)
		wanted_heap = max_heap;
	if (wanted_heap < 88 * R615_MIB)
		return -ENOSPC;
	if (!r615_wpr_below(meta.elf_start, wanted_heap, R615_MIB, &meta.heap_start) ||
	    !r615_wpr_below(meta.heap_start, R615_MIB, R615_MIB, &meta.wpr_start) ||
	    !r615_wpr_below(meta.wpr_start, R615_MIB, R615_MIB, &meta.non_wpr_start))
		return -ENOSPC;
	if (meta.fb_bytes - meta.non_wpr_start > R615_PRESCRUB_BYTES)
		return -ENOSPC;
	meta.heap_bytes = (meta.elf_start - meta.heap_start) & ~(R615_MIB - 1);
	meta.non_wpr_bytes = R615_MIB;
	meta.reserved_start = meta.non_wpr_start;
	meta.elf_radix = config.elf_radix;
	meta.boot_address = config.boot_address;
	meta.boot_code = config.boot_code;
	meta.boot_data = config.boot_data;
	meta.boot_manifest = config.boot_manifest;
	meta.signature_address = config.signature_address;
	meta.signature_bytes = config.signature_bytes;
	meta.boot_count = 0;
	/* Verified stays zero: Booter authenticates and locks the WPR. */
	meta.verified = 0;
	meta.crash_address = config.crash_address;
	meta.crash_bytes = config.crash_bytes;
	meta.flags = config.boost ? 1 : 0;
	*output = meta;
	return 0;
}

u64
r615_libos_id(const char *name)
{
	u64 id = 0;

	if (!name)
		return 0;
	for (u32 i = 0; i < 8 && name[i]; i++)
		id = id << 8 | (u8)name[i];
	return id;
}

int
r615_libos_build(const struct r615_libos_region *regions, u32 count, void *output, size_t bytes)
{
	bool rmargs = false;

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	return -EOPNOTSUPP;
#endif
	/* Leave a zero terminator even if the firmware scans the entire page. */
	if (!regions || !output || !count || count >= R615_BOOT_PAGE / sizeof(*regions) ||
	    bytes != R615_BOOT_PAGE || regions[0].id != r615_libos_id("LOGINIT") ||
	    r615_bootmem_overlap(output, bytes, regions, (size_t)count * sizeof(*regions)))
		return -EINVAL;
	for (u32 i = 0; i < count; i++) {
		if (!regions[i].id || regions[i].kind != 1 || regions[i].location != 1 ||
		    !r615_bootmem_dma(regions[i].address, regions[i].bytes) ||
		    regions[i].bytes % R615_BOOT_PAGE)
			return -EINVAL;
		for (u32 j = 0; j < sizeof(regions[i].reserved); j++) {
			if (regions[i].reserved[j])
				return -EINVAL;
		}
		if (regions[i].id == r615_libos_id("RMARGS")) {
			if (regions[i].bytes != R615_BOOT_PAGE)
				return -EINVAL;
			rmargs = true;
		}
		for (u32 j = 0; j < i; j++) {
			if (regions[j].id == regions[i].id ||
			    (regions[i].address < regions[j].address + regions[j].bytes &&
			     regions[j].address < regions[i].address + regions[i].bytes))
				return -EINVAL;
		}
	}
	if (!rmargs)
		return -EINVAL;
	memset(output, 0, bytes);
	memcpy(output, regions, (size_t)count * sizeof(*regions));
	return 0;
}
