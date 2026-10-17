/* SPDX-License-Identifier: MIT */
/* Interface excerpts: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. MIT permission notice: LICENSES/preferred/MIT.
 * Public 615 gsp_fw_sr_meta.h, TU102 kgspPrepareSuspendResumeData.
 */
#ifndef __NVKM_R615_SR_H__
#define __NVKM_R615_SR_H__
#include "bootmem.h"

#define R615_SR_MAGIC 0x8a3bb9e6c6c39d93ULL
#define R615_SR_REVISION 2ULL
#define R615_SR_MAX_BYTES (256U << 20)
struct r615_sr_meta {
	u64 magic, revision, radix, bytes;
	u32 internal[32], flags, subrevision, padding[22];
};
int r615_sr_size(const struct r615_wpr_meta *, u64 *bytes);
int r615_sr_meta_build(const struct r615_wpr_meta *, u64 radix, u64 bytes,
		       struct r615_sr_meta *);

struct device;
struct r615_sr;
int r615_sr_new(struct device *, const struct r615_wpr_meta *, struct r615_sr **);
/* Publishing either to FBSR_INIT or to a Booter is an exposure, even if the
 * subsequent command fails. Failed stop must retain the entire resource.
 */
int r615_sr_publish(struct r615_sr *, u64 *metadata_address);
/* Owner must prove ALL GPU users stopped; a software running flag is not proof. */
void r615_sr_quiesced(struct r615_sr *);
void r615_sr_quarantine(struct r615_sr *);
bool r615_sr_dma_owned(const struct r615_sr *, u64 address, u64 bytes);
int r615_sr_del(struct r615_sr **);
#endif
