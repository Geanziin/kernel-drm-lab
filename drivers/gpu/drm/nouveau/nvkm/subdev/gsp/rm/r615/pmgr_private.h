/* SPDX-License-Identifier: MIT */
/* NVIDIA-DERIVED: private local use only, not for redistribution/upstream.
 * See pmgr_private.c for provenance.
 */
#ifndef __NVKM_GSP_R615_PMGR_PRIVATE_H__
#define __NVKM_GSP_R615_PMGR_PRIVATE_H__
#include <linux/types.h>

#define R615_PMGR_POLICY_INFO_CTRL 0x2080a618U
#define R615_PMGR_POLICY_INFO_BYTES 0x5020U

struct r615_pmgr_limits { u32 min_mw, rated_mw, max_mw; };

/* Parse a POLICY_INFO reply (pure, CPU-testable). */
int r615_pmgr_parse_limits(const u8 *info, size_t bytes, struct r615_pmgr_limits *);

struct nvkm_gsp;
int r615_pmgr_query_limits(struct nvkm_gsp *, struct r615_pmgr_limits *);
#endif
