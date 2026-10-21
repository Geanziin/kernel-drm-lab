/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R535_HYBRID615_H__
#define __NVKM_R535_HYBRID615_H__

#include <linux/string.h>

#include <nvkm/subdev/gsp.h>

/* Candidate 10 hybrid: 615 deltas on the proven 570 boot path. GA107 +
 * 615.78.08 only; every use is version-gated, legacy entries never take
 * these branches.
 */
static inline bool
r535_gsp_hybrid615(struct nvkm_gsp *gsp)
{
	return gsp && gsp->rm && gsp->rm->version &&
	       !strcmp(gsp->rm->version, "615.78.08") &&
	       gsp->subdev.device && gsp->subdev.device->chipset == 0x177;
}

/* GSP queue registers (TU102 HAL, also selected for GA107 in 615). */
#define HYBRID615_MSGQ_TX_HEAD 0x110c00
#define HYBRID615_MSGQ_TX_TAIL 0x110c04
#define HYBRID615_MSGQ_RX_HEAD 0x110c80
#define HYBRID615_MSGQ_RX_TAIL 0x110c84

#endif
