/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Minimal GMCAPI NVKV system-info encode / static-info decode for GA107.
 * Key numbers are interface facts from public 615.78.08 headers
 * (gmcapi_system_info.h, gmcapi_gsp_config.h, MIT). No firmware code copied.
 * Baremetal cold-boot subset only; VGPU/ACPI/SRIOV/registry land later.
 */
#ifndef __NVKM_R615_SYSINFO_H__
#define __NVKM_R615_SYSINFO_H__

#include <linux/bits.h>
#include <linux/types.h>

/* System-info keys (CPU -> GSP). */
#define R615_SI_PCI_DEVICE_ID      0x0001u
#define R615_SI_PCI_SUB_DEVICE_ID  0x0002u
#define R615_SI_PCI_REVISION_ID    0x0003u
#define R615_SI_PCI_MIRROR_BASE    0x0010u
#define R615_SI_PCI_MIRROR_SIZE    0x0011u
#define R615_SI_HOST_ARCH          0x0070u
#define R615_SI_HOST_ARCH_X86_64   1u
#define R615_SI_HOST_OS_TYPE       0x0071u
#define R615_SI_HOST_OS_UNIX       1u
#define R615_SI_HOST_PAGE_SIZE     0x0078u
#define R615_SI_PCIE_FLAGS         0x1000u
#define R615_SI_PLATFORM_FLAGS     0x1001u
#define R615_SI_PLATFORM_PRIMARY   BIT_ULL(2)
#define R615_SI_FEATURE_FLAGS      0x1002u
#define R615_SI_VF_FLAGS           0x1003u
#define R615_SI_GPU_PHYS_ADDR      0x1010u
#define R615_SI_GPU_PHYS_FB        0x1011u
#define R615_SI_GPU_PHYS_INST      0x1012u
#define R615_SI_GPU_PHYS_IO        0x1013u
#define R615_SI_DOMAIN_BDF         0x1020u
#define R615_SI_ATOMICS_OP_MASK    0x1030u
#define R615_SI_CONSOLE_MEM        0x1031u
#define R615_SI_MAX_USER_VA        0x1032u
#define R615_SI_DRIVER_NAME        0x1100u
#define R615_SI_DRIVER_VERSION     0x1101u
#define R615_SI_DRIVER_CHANGELIST  0x1102u
#define R615_SI_UNSTABLE           0x1110u
#define R615_SI_UNSTABLE_RPC       BIT(0)
#define R615_SI_UNSTABLE_EVENTS    BIT(1)

/* Static-info keys (GSP -> CPU), minimal subset for nouveau bringup. */
#define R615_SC_INTERNAL_CLIENT    0x0ff0u
#define R615_SC_INTERNAL_DEVICE    0x0ff1u
#define R615_SC_INTERNAL_SUBDEVICE 0x0ff2u
#define R615_SC_FB_REGION_COUNT    0x0010u
#define R615_SC_FB_REGION_FLAGS    0x0012u
#define R615_SC_FB_REGION_TAG      0x0013u
#define R615_SC_FB_REGION_BASE     0x1011u
#define R615_SC_FB_REGION_LIMIT    0x1012u
#define R615_SC_FB_REGION_RESERVED 0x1013u
#define R615_SC_BAR1_PDB           0x1020u
#define R615_SC_BAR2_PDB           0x1021u
#define R615_SC_FB_LENGTH          0x1010u

#define R615_SYSINFO_MAX_WORDS 256u
#define R615_STATIC_MAX_REGIONS 16u

struct r615_sysinfo {
	u32 pci_device, pci_sub, pci_rev;
	u64 phys_bar0, phys_fb, phys_inst;
	u64 domain_bdf;
	u32 cfg_base, cfg_size;
	bool primary;
	u64 console_mem, max_user_va;
};

struct r615_fb_region {
	u64 base, limit, reserved;
	u32 flags, tag;
	bool flags_valid, tag_valid;
};

/* Compression + ISO, unprotected, no reservation or RM/carveout tag.
 * Missing classification is not permission to hand VRAM to the host allocator.
 */
static inline bool
r615_fb_region_usable(const struct r615_fb_region *region)
{
	return region->flags_valid && region->tag_valid && region->flags == 3 &&
	       !region->reserved && !region->tag;
}

struct r615_static {
	u32 client, device, subdevice;
	u64 bar1_pdb, bar2_pdb;
	u64 fb_length;
	u32 region_count;
	struct r615_fb_region regions[R615_STATIC_MAX_REGIONS];
};

/* NVKV encode/decode using r615_nvkv_*; pure memory, no DMA/MMIO. */
int r615_sysinfo_encode(const struct r615_sysinfo *in, u64 *words,
			u32 limit, u32 *count);
int r615_static_decode(const u64 *words, u32 count, struct r615_static *out);

#endif
