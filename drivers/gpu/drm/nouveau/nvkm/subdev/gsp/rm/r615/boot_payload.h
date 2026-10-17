/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_BOOT_PAYLOAD_H__
#define __NVKM_R615_BOOT_PAYLOAD_H__

#include "bootmem.h"
#include "transport.h"

struct firmware;
struct r615_boot_payload;
void r615_boot_payload_quarantine(struct r615_boot_payload *);
#define R615_BOOT_LOG_COUNT 8U

struct r615_boot_environment {
	u64 fb_bytes, vga_start, mmu_lock_start;
	bool vga_valid, mmu_lock_valid, boost;
};

struct r615_boot_handoff {
	u64 wpr_address;	/* WPR V1 metadata passed to SEC2 Booter */
	u64 libos_address;	/* LibOS region table passed in GSP mailboxes */
	u64 args_address;	/* GSP-RM boot arguments named by the RMARGS entry */
	u64 state_monitor_address; /* host diagnostics, not a new firmware ABI */
	u32 state_monitor_bytes;
	u32 app_version;
};

/* Sleepable, serialized by the boot owner; never call with the GSP running. */
int r615_boot_payload_new(struct nvkm_gsp *gsp, const struct firmware *firmware,
			  const struct firmware *bootloader, const struct firmware *ucodes,
			  const struct r615_boot_environment *environment,
			  r615_transport_quiesce_func quiesce, void *context,
			  struct r615_boot_payload **output);
int r615_boot_payload_request(struct nvkm_gsp *gsp,
			      const struct r615_boot_environment *environment,
			      r615_transport_quiesce_func quiesce, void *context,
			      struct r615_boot_payload **output);
/* Prepares handoff only; caller still needs FWSEC/Booter, mailboxes and RPC init. */
int r615_boot_payload_arm(struct r615_boot_payload *payload, struct r615_boot_handoff *output);
struct r615_transport *r615_boot_payload_transport(struct r615_boot_payload *payload);
int r615_boot_payload_wpr(struct r615_boot_payload *payload, struct r615_wpr_meta *output);
int r615_boot_payload_resume_info(const struct r615_boot_payload *, u64 *libos, u32 *version);
bool r615_boot_payload_dma_owned(const struct r615_boot_payload *, u64 address, u64 bytes);
/* Only after processor/Booter teardown and IRQ/work drain; keep the entire
 * payload for a real PM resume, not the terminal quiesce/free path.
 */
int r615_boot_payload_pm_pause(struct r615_boot_payload *);
/* Retained queues and allocation addresses; old_level is public D3 (3) or
 * hibernate (7). This does not run Booter or complete FBSR restoration.
 */
int r615_boot_payload_pm_resume(struct r615_boot_payload *, u32 old_level,
				struct r615_boot_handoff *);
const void *r615_boot_payload_log_init(const struct r615_boot_payload *payload, size_t *bytes);
const void *r615_boot_payload_log(const struct r615_boot_payload *payload, u32 index,
				const char **name, size_t *bytes);
int r615_boot_payload_quiesce(struct r615_boot_payload *payload);
int r615_boot_payload_del(struct r615_boot_payload **payload);

#endif
