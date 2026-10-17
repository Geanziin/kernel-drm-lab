/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_MESSAGE_H__
#define __NVKM_R615_MESSAGE_H__
#include "msgq.h"

#define R615_MESSAGE_RPC 0x25U
#define R615_MESSAGE_GMCAPI 0x26U
#define R615_MESSAGE_MAX_BYTES (R615_MSGQ_PAGE_BYTES * R615_MSGQ_MAX_SLOTS)

struct r615_message {
	u32 function;
	u64 sequence;
	u32 status_or_limit;
	u32 private_status;
	u32 gfid;
	u32 bytes;
	const void *data;
	u8 type;
	bool response;
};

/*
 * Raw wire framing only. It neither correlates replies nor handles continuation
 * records or dispatches events. Parsed data borrows the caller's staging buffer.
 */
int r615_message_build(const struct r615_message *message, void *output, size_t capacity,
		       u32 *slot_bytes);
int r615_message_parse(const void *buffer, size_t available, struct r615_message *output);
int r615_message_send(struct r615_msgq *queue, const struct r615_message *message,
		      void *staging, size_t capacity);
int r615_message_receive(struct r615_msgq *queue, void *staging, size_t capacity,
			 struct r615_message *output);

#endif
