/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_BOOTMEM_H__
#define __NVKM_R615_BOOTMEM_H__

#include <linux/types.h>

#define R615_BOOT_PAGE 4096U
#define R615_RADIX_ENTRIES 512U
#define R615_WPR_MAGIC 0xdc3aae21371a60b3ULL
#define R615_WPR_REVISION 1ULL

struct r615_radix3_plan {
	u64 data_bytes;
	u32 data_pages, directory_pages, leaf_pages, table_bytes;
};

/*
 * 615.78.08 GspFwWprMetaV1: Booter uses this on Turing through Ada,
 * including GA107. The GH100+ FMC handoff is not the GA107 boot ABI.
 */
struct r615_wpr_meta {
	u64 magic, revision;
	u64 elf_radix, elf_bytes;			/* sysmem addresses */
	u64 boot_address, boot_bytes;
	u64 boot_code, boot_data, boot_manifest;
	u64 signature_address, signature_bytes;
	u64 reserved_start;				/* gspFwRsvdStart */
	u64 non_wpr_start, non_wpr_bytes;
	u64 wpr_start;
	u64 heap_start, heap_bytes;			/* GSP-RM heap */
	u64 elf_start;
	u64 boot_start;
	u64 frts_start, frts_bytes;
	u64 wpr_end;
	u64 fb_bytes;
	u64 vga_start, vga_bytes;
	u64 boot_count;
	u8 reserved1[16];
	u64 crash_address;
	u32 crash_bytes, reserved2;
	u8 vf_count, flags;
	u16 paging_config;
	u32 reserved3;
	u64 verified;
};

struct r615_wpr_config {
	u32 chipset;
	u64 fb_bytes, vga_start, mmu_lock_start;
	bool vga_valid, mmu_lock_valid, boost;
	u64 elf_radix, elf_bytes, boot_address, boot_bytes;
	u64 boot_code, boot_data, boot_manifest;
	u64 signature_address, signature_bytes;
	u64 crash_address;
	u32 crash_bytes;
};

struct r615_libos_region {
	u64 id, address, bytes;
	u8 kind, location, reserved[6];
};

/* These produce memory only; neither allocate DMA nor release the GSP reset. */
int r615_radix3_plan(u64 data_bytes, struct r615_radix3_plan *output);
int r615_radix3_build(const struct r615_radix3_plan *plan, void *tables, size_t bytes,
		      u64 table_address, const u64 *data_addresses, u32 data_pages);
int r615_wpr_build(const char *version, const struct r615_wpr_config *config,
		   struct r615_wpr_meta *output);
u64 r615_libos_id(const char *name);
int r615_libos_build(const struct r615_libos_region *regions, u32 count,
		     void *output, size_t bytes);

#endif
