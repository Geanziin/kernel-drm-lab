/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_MSGQ_H__
#define __NVKM_R615_MSGQ_H__

#include "boot.h"

#define R615_MSGQ_PAGE_BYTES 4096U
#define R615_MSGQ_QUEUE_BYTES 0x40000U
#define R615_MSGQ_SHARED_BYTES (2U * R615_MSGQ_QUEUE_BYTES + R615_MSGQ_PAGE_BYTES)
#define R615_MSGQ_PTE_COUNT (R615_MSGQ_SHARED_BYTES / R615_MSGQ_PAGE_BYTES)
#define R615_MSGQ_MAX_SLOTS 16U
#define R615_MSGQ_ELEMENT_HEADER_BYTES 24U

struct r615_msgq_header {
	u16 major, minor;
	u32 bytes, slot_bytes, slots, entry_offset;
	u32 reserved[3];
};

enum r615_msgq_register {
	R615_MSGQ_TX_HEAD, R615_MSGQ_TX_TAIL, R615_MSGQ_RX_HEAD, R615_MSGQ_RX_TAIL
};

struct r615_msgq_ops {
	int (*read)(void *context, enum r615_msgq_register reg, u32 *value);
	int (*write)(void *context, enum r615_msgq_register reg, u32 value);
	void (*acquire)(void *context);
	void (*publish)(void *context);
};

/*
 * Caller serializes access and supplies coherent memory; publish orders reads
 * and writes before MMIO. Buffers cannot be freed until DMA is quiesced.
 */
struct r615_msgq {
	void *memory;
	const struct r615_msgq_ops *ops;
	void *context;
	u32 tx_counter, rx_counter;
	/* Last plausible peer counters (public msgq txReadPtr / rxWritePtr). */
	u32 tx_peer, rx_peer;
	u32 resets; /* peer register reads that were impossible and replaced */
	bool tx_started, rx_linked, failed;
};

int r615_msgq_prepare(struct r615_msgq *queue, const char *version, void *memory,
		      size_t bytes, u64 dma_address, const struct r615_msgq_ops *ops, void *context,
		      struct r615_init_args *output);
int r615_msgq_start(struct r615_msgq *queue);
int r615_msgq_link_rx(struct r615_msgq *queue);
int r615_msgq_send(struct r615_msgq *queue, const void *buffer, size_t bytes);
int r615_msgq_peek(struct r615_msgq *queue, void *buffer, size_t bytes);
int r615_msgq_consume(struct r615_msgq *queue, u32 slots);
int r615_msgq_consume_frame(struct r615_msgq *queue, u32 slots);

#endif
