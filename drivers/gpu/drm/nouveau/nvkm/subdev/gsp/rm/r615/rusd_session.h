/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_RUSD_SESSION_H__
#define __NVKM_R615_RUSD_SESSION_H__

#include "rusd_producer.h"

struct nvkm_gsp;
struct r615_rusd_session;

int r615_rusd_session_new(struct nvkm_gsp *, struct r615_rusd_session **);
int r615_rusd_session_start(struct r615_rusd_session *, u64 poll_mask, u32 interval_ms);
int r615_rusd_session_stop(struct r615_rusd_session *);
int r615_rusd_session_read(struct r615_rusd_session *, enum r615_rusd_metric,
			   u64 verified_now_ns, u64 max_age_ns, struct r615_rusd_value *);
/* The port must first prove that GSP DMA is stopped, not only clear running. */
int r615_rusd_session_quiesced(struct r615_rusd_session *);
/* On failed unmap the session stays owned by the caller, with DMA pinned. */
int r615_rusd_session_del(struct r615_rusd_session **);

#endif
