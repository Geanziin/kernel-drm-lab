/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_TRANSPORT_H__
#define __NVKM_R615_TRANSPORT_H__

#include "message.h"

struct nvkm_gsp;
struct r615_transport;
void r615_transport_quarantine(struct r615_transport *);

/*
 * Sleepable boot/worker context only, never hard IRQ. The boot owner keeps the
 * GSP/device alive and serializes creation/deletion with all transport users.
 */

/*
 * Boot owner must stop the GSP and all its DMA, revoke boot-argument aliases,
 * and drain IRQ/work that can access this transport before returning zero.
 * A cleared software running flag is not sufficient. Called without the transport
 * mutex, after blocking further queue operations, so workers can be drained
 * without deadlocking on that mutex. Failure retains all memory.
 */
typedef int (*r615_transport_quiesce_func)(struct nvkm_gsp *gsp, void *context);

int r615_transport_new(struct nvkm_gsp *gsp, r615_transport_quiesce_func quiesce,
		       void *context, struct r615_transport **output);
/* Call before releasing the GSP from reset; arguments are exposed only here. */
int r615_transport_arm(struct r615_transport *transport, struct r615_init_args *output);
int r615_transport_link_rx(struct r615_transport *transport);
int r615_transport_send(struct r615_transport *transport, const struct r615_message *message);
/* Parsed payload borrows frame (not internal memory); caller owns its lifetime. */
int r615_transport_receive(struct r615_transport *transport, void *frame, size_t capacity,
			   struct r615_message *output);
int r615_transport_quiesce(struct r615_transport *transport);
/* Nonterminal PM hold: same hardware/worker-stop proof as quiesce, but retain
 * queue contents, counters and linkage for firmware restore. Never re-prepare.
 */
int r615_transport_pm_pause(struct r615_transport *);
int r615_transport_pm_args(struct r615_transport *, struct r615_init_args *);
int r615_transport_pm_resume(struct r615_transport *);
/* Refuses to free exposed DMA until quiesce succeeded; does not stop hardware. */
int r615_transport_del(struct r615_transport **transport);

#endif
