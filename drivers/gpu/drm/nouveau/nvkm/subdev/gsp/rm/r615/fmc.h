/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_FMC_H__
#define __NVKM_R615_FMC_H__

#include <linux/types.h>

/*
 * Historical prototype, NOT used by GA107 or built into the lab module.
 * The GA107 HAL selects TU102 Booter/WPR V1, not GH100+ FMC arguments.
 * Kept for auditing candidate 06; CPU layout tests do not establish applicability.
 *
 * GSP-FMC (ACR) boot arguments for 615.78.08.
 *
 * The host stages one of these pages in coherent system memory and hands its
 * physical address to the secure Falcon loader through MAILBOX0/MAILBOX1. The
 * loader passes the block on to the GSP-FMC, which reports its status back in
 * MAILBOX0 (zero means success) before GSP-RM is released.
 */
struct r615_fmc_config {
	u64 rm_desc_offset;	/* WPR metadata visible to the GSP-FMC */
	u32 rm_desc_size;
	u64 boot_args_offset;	/* GSP-RM boot arguments (LibOS init args) */
	bool scrub_cbc_sr;	/* cold boot / FLR / driver reload */
	bool sr_resume;
};

struct r615_fmc_boot_gsp_rm {
	u32 target;			/* aperture the offsets below live in */
	u32 rm_desc_size;
	u64 rm_desc_offset;
	u64 wpr_carveout_offset;
	u32 wpr_carveout_size;
	u8 is_rm_boot;
	u8 inst_in_sys_mode;
	u8 icu_enabled;
	u8 scrub_cbc_sr;
};

struct r615_fmc_rm_params {
	u32 target;
	u32 reserved;
	u64 boot_args_offset;
};

struct r615_fmc_spdm_params {
	u32 target;
	u32 payload_size;
	u64 payload_offset;
};

struct r615_fmc_rm_mem_params {
	u32 flush_sysmem_addr_lo;
	u32 flush_sysmem_addr_hi;
};

struct r615_fmc_boot_params {
	u32 magic;
	u16 size;
	u16 reserved0;
	u64 reserved1[3];

	u32 regkeys;
	u32 reserved_init;
	u64 reserved2[3];

	struct r615_fmc_boot_gsp_rm boot_gsp_rm;
	u64 reserved3[4];

	struct r615_fmc_rm_params gsp_rm;
	u64 reserved4[2];

	struct r615_fmc_spdm_params gsp_spdm;
	u64 reserved5[2];

	struct r615_fmc_rm_mem_params gsp_rm_mem;
	u64 reserved6[7];
};

struct r615_fmc_error {
	u32 variant, partition, payload;
	u32 sk_error, sk_phase;
	u32 code, additional;
};

/* Falcon MAILBOX0 value the reference writes before starting the booter. */
#define R615_FLCN_ERR_BINARY_NOT_STARTED 0xfeu

int r615_fmc_build(const struct r615_fmc_config *config, void *page, size_t bytes);
const char *r615_fmc_describe(u32 mailbox0, u32 mailbox1, u32 args_low, u32 args_high,
			      struct r615_fmc_error *error);

#endif
