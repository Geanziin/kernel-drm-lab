/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_TELEMETRY_H__
#define __NVKM_R615_TELEMETRY_H__
#include <linux/types.h>
#include "rusd.h"

struct nvkm_gsp;

/* Poll groups requested from GSP-RM (public cl00de.h NV00DE_RUSD_POLL_*). */
#define R615_TELEMETRY_POLL_MASK (0x01 /* clock */ | 0x02 /* perf */ | \
				  0x08 /* power */ | 0x10 /* thermal */)
/* Public 615 nvrm_registry.h NV_REG_STR_RM_RUSD_POLLING_INTERVAL_DEFAULT. */
#define R615_TELEMETRY_INTERVAL_MS 500U
/* A group whose timestamp did not advance for this long (host monotonic) is
 * reported stale: the producer's timestamp clock domain is not public, so
 * freshness is judged by observed progress, never by comparing clocks.
 */
#define R615_TELEMETRY_STALE_MS (8U * R615_TELEMETRY_INTERVAL_MS)

int r615_telemetry_start(struct nvkm_gsp *);
/* Sends the unmap while GSP still runs; memory kept if not acknowledged. */
int r615_telemetry_stop(struct nvkm_gsp *);
/* Call only after GSP DMA is proven stopped. */
void r615_telemetry_quiesced(struct nvkm_gsp *);
void r615_telemetry_del(struct nvkm_gsp *);
int r615_telemetry_read(struct nvkm_gsp *, enum r615_rusd_metric, s64 *value);
/* Physical video-engine clock busy %, via a direct perf control (not RUSD):
 * NVENC/NVDEC usage by nouveau/NVK is not reflected in the RUSD engUtil.
 * encoder=true -> NVENC, false -> NVDEC. Returns -ENODEV when GSP is absent.
 */
int r615_telemetry_vid_eng(struct nvkm_gsp *, bool encoder, s64 *percent);
/* Max settable power limit (µW) via the private PMGR power-limit control.
 * Returns -ENODEV/-EOPNOTSUPP when GSP or the limit is unavailable.
 */
int r615_telemetry_power_cap_max(struct nvkm_gsp *, s64 *microwatt);

#endif
