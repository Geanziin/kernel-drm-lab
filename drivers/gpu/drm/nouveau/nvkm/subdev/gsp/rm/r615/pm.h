/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_PM_H__
#define __NVKM_R615_PM_H__
#include "bootmem.h"
#include "sysinfo.h"

struct r615_pm_plan { u64 backup_bytes; u32 pages; };
/* Public 615 FBSR reserved-range upper bound; requires typed region metadata. */
int r615_pm_plan(const struct r615_wpr_meta *, const struct r615_static *, u64 cbc_bytes,
		 struct r615_pm_plan *);
struct nvkm_gsp;
struct r615_pm;
int r615_pm_suspend(struct nvkm_gsp *);
int r615_pm_resume(struct nvkm_gsp *);
bool r615_pm_sleeping(const struct r615_pm *);
/* A failed exposed transition is retained even after GSP-only teardown:
 * it does not prove outstanding CE DMA stopped. Quarantine the entire owner.
 */
int r615_pm_del(struct nvkm_gsp *);
bool r615_pm_dma_owned(const struct r615_pm *, u64, u64);
int r615_gsp_teardown_pm(struct nvkm_gsp *, u64 sr_metadata);
int r615_gsp_bootstrap_pm(struct nvkm_gsp *, u64 sr_metadata, u32 app_version);
#endif
