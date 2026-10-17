// SPDX-License-Identifier: MIT
/*
 * GSP-FMC (ACR) boot arguments and Falcon mailbox status for 615.78.08.
 *
 * Independent implementation written against the published 615 GSP interface
 * (GSP_FMC_BOOT_PARAMS: magic 'FMC ', 256 bytes) and the published Falcon
 * sequence: the secure loader takes the argument block address in
 * MAILBOX0/MAILBOX1, and MAILBOX0 carries the booter's 8-bit status code,
 * where zero means success and a leftover argument pointer means the binary
 * never started. No proprietary code is reproduced here.
 */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#include "bootmem.h"
#include "fmc.h"

#define R615_FMC_MAGIC (' ' << 24 | 'C' << 16 | 'M' << 8 | 'F')

enum r615_fmc_dma_target {
	R615_DMA_TARGET_LOCAL_FB = 0,
	R615_DMA_TARGET_COHERENT_SYSTEM = 1,
	R615_DMA_TARGET_NONCOHERENT_SYSTEM = 2,
};

#define R615_FMC_ERROR_VARIANT_SK 0u
#define R615_FMC_ERROR_VARIANT_GENERIC 1u

_Static_assert(sizeof(struct r615_fmc_boot_gsp_rm) == 32, "FMC boot params size");
_Static_assert(sizeof(struct r615_fmc_boot_params) == 256, "FMC boot params size");

int
r615_fmc_build(const struct r615_fmc_config *config, void *page, size_t bytes)
{
	struct r615_fmc_boot_params params = { 0 };

	if (!config || !page)
		return -EINVAL;
	if (bytes < sizeof(params) || !config->rm_desc_offset || !config->rm_desc_size ||
	    !config->boot_args_offset || (unsigned long)page % R615_BOOT_PAGE)
		return -EINVAL;

	params.magic = R615_FMC_MAGIC;
	params.size = sizeof(params);
	/* This block is staged with dma_alloc_coherent(). */
	params.boot_gsp_rm.target = R615_DMA_TARGET_COHERENT_SYSTEM;
	params.boot_gsp_rm.rm_desc_size = config->rm_desc_size;
	params.boot_gsp_rm.rm_desc_offset = config->rm_desc_offset;
	params.boot_gsp_rm.is_rm_boot = 1;
	params.boot_gsp_rm.icu_enabled = 0;
	params.boot_gsp_rm.inst_in_sys_mode = 0;
	params.boot_gsp_rm.scrub_cbc_sr = config->scrub_cbc_sr;
	/* The WPR is placed by the GSP-FMC from the RM descriptor. */
	params.boot_gsp_rm.wpr_carveout_offset = 0;
	params.boot_gsp_rm.wpr_carveout_size = 0;

	params.gsp_rm.target = R615_DMA_TARGET_COHERENT_SYSTEM;
	params.gsp_rm.boot_args_offset = config->boot_args_offset;

	memcpy(page, &params, sizeof(params));
	return 0;
}

/*
 * Classify what MAILBOX0 says after the loader halted. A value that still
 * matches the argument address means the binary never ran; zero means the
 * booter succeeded; anything else is the booter's 8-bit status code, split
 * into variant / partition / payload.
 */
const char *
r615_fmc_describe(u32 mailbox0, u32 mailbox1, u32 args_low, u32 args_high,
		  struct r615_fmc_error *error)
{
	struct r615_fmc_error local = { 0 };
	struct r615_fmc_error *out = error ? error : &local;

	out->variant = mailbox0 >> 28;
	out->partition = (mailbox0 >> 24) & 0xf;
	out->payload = mailbox0 & 0xffffff;
	out->sk_error = out->payload >> 8;
	out->sk_phase = out->payload & 0xff;
	out->additional = out->payload >> 16;
	out->code = out->payload & 0xffff;

	if (mailbox0 == args_low && mailbox1 == args_high)
		return "argument pointer unchanged, binary did not start";
	if (mailbox0 == R615_FLCN_ERR_BINARY_NOT_STARTED)
		return "binary not started";
	if (mailbox0 == 0)
		return "ok";
	if (out->variant == R615_FMC_ERROR_VARIANT_SK)
		return "booter error (SK variant)";
	if (out->variant == R615_FMC_ERROR_VARIANT_GENERIC)
		return "booter error (generic variant)";
	return "booter error (unknown variant)";
}
