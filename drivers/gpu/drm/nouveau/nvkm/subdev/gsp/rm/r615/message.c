// SPDX-License-Identifier: MIT
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "message.h"

#define R615_MCTP_MAGIC 0x4d435450U
#define R615_MCTP_SEEN 0x5345454eU
#define R615_MCTP_SINGLE 0xc0000001U
#define R615_MCTP_NVIDIA 0x0010de7eU
#define R615_RPC_VERSION 0x03000000U
#define R615_RPC_SIGNATURE 0x43505256U
#define R615_GMC_RESPONSE 0x01000000U

struct r615_message_outer {
	u32 magic, total_bytes, mctp, nvdm, payload_bytes, reserved;
};

struct r615_message_rpc {
	u32 version, signature, bytes, function, status, private_status, sequence, gfid;
};

struct r615_message_gmc {
	u32 command, bytes;
	u64 sequence;
	u32 status_or_limit;
	u32 reserved[5];
};

static_assert(sizeof(struct r615_message_outer) == 24, "615 plaintext envelope");
static_assert(sizeof(struct r615_message_rpc) == 32, "615 RPC header v3");
static_assert(sizeof(struct r615_message_gmc) == 40, "615 GMCAPI header");

static bool
r615_message_overlap(const void *left, size_t left_bytes, const void *right, size_t right_bytes)
{
	unsigned long a = (unsigned long)left, b = (unsigned long)right;

	if (a > ~0UL - left_bytes || b > ~0UL - right_bytes)
		return true;
	return left_bytes && right_bytes && a < b + right_bytes && b < a + left_bytes;
}

int
r615_message_build(const struct r615_message *message, void *output, size_t capacity,
		   u32 *slot_bytes)
{
	struct r615_message_outer outer = {
		.magic = R615_MCTP_MAGIC, .mctp = R615_MCTP_SINGLE,
	};
	struct r615_message_rpc rpc = { 0 };
	struct r615_message_gmc gmc = { 0 };
	struct r615_message input;
	u32 header_bytes, needed;

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	return -EOPNOTSUPP;
#endif
	if (!message || !output || !slot_bytes || (!message->data && message->bytes))
		return -EINVAL;
	input = *message;
	if (input.type == R615_MESSAGE_RPC) {
		if (input.sequence > 0xffffffffULL || input.response)
			return -EINVAL;
		header_bytes = sizeof(rpc);
		rpc.version = R615_RPC_VERSION;
		rpc.signature = R615_RPC_SIGNATURE;
		rpc.function = input.function;
		rpc.status = input.status_or_limit;
		rpc.private_status = input.private_status;
		rpc.sequence = input.sequence;
		rpc.gfid = input.gfid;
	} else if (input.type == R615_MESSAGE_GMCAPI) {
		if (input.function <= 0xffff || input.function > 0xffffff ||
		    input.private_status || input.gfid)
			return -EINVAL;
		header_bytes = sizeof(gmc);
		gmc.command = input.function | (input.response ? R615_GMC_RESPONSE : 0);
		gmc.sequence = input.sequence;
		gmc.status_or_limit = input.status_or_limit;
	} else {
		return -EOPNOTSUPP;
	}
	if (input.bytes > R615_MESSAGE_MAX_BYTES - sizeof(outer) - header_bytes)
		return -EMSGSIZE;
	outer.payload_bytes = header_bytes + input.bytes;
	outer.total_bytes = sizeof(outer) + outer.payload_bytes;
	outer.nvdm = R615_MCTP_NVIDIA | (u32)input.type << 24;
	needed = (outer.total_bytes + R615_MSGQ_PAGE_BYTES - 1) & ~(R615_MSGQ_PAGE_BYTES - 1);
	if (capacity < needed)
		return -ENOSPC;
	if (r615_message_overlap(output, needed, input.data, input.bytes) ||
	    r615_message_overlap(output, needed, slot_bytes, sizeof(*slot_bytes)))
		return -EINVAL;
	memset(output, 0, needed);
	memcpy(output, &outer, sizeof(outer));
	if (input.type == R615_MESSAGE_RPC) {
		rpc.bytes = outer.payload_bytes;
		memcpy((u8 *)output + sizeof(outer), &rpc, sizeof(rpc));
	} else {
		gmc.bytes = input.bytes;
		memcpy((u8 *)output + sizeof(outer), &gmc, sizeof(gmc));
	}
	if (input.bytes)
		memcpy((u8 *)output + sizeof(outer) + header_bytes, input.data, input.bytes);
	*slot_bytes = needed;
	return 0;
}

int
r615_message_parse(const void *buffer, size_t available, struct r615_message *output)
{
	struct r615_message_outer outer;
	struct r615_message message = { 0 };
	u32 header_bytes;

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	return -EOPNOTSUPP;
#endif
	if (!buffer || !output || available < sizeof(outer) ||
	    r615_message_overlap(buffer, available, output, sizeof(*output)))
		return -EINVAL;
	memcpy(&outer, buffer, sizeof(outer));
	if (outer.magic == R615_MCTP_SEEN)
		return -ESTALE;
	/* Public 615 GspMsgQueueReceiveStatus checks magic, MCTP version and
	 * NVDM vendor/type only; reserved words are not validated. Keep every
	 * size relation that bounds memory access, accept what the host accepts.
	 */
	if (outer.magic != R615_MCTP_MAGIC ||
	    outer.total_bytes < sizeof(outer) || outer.total_bytes > R615_MESSAGE_MAX_BYTES ||
	    outer.payload_bytes > outer.total_bytes - sizeof(outer))
		return -EPROTO;
	/* Single-packet only: version 1 with SOM and EOM. Tag/seq bits ignored. */
	if ((outer.mctp & 0xc000000fU) != R615_MCTP_SINGLE ||
	    (outer.nvdm & 0xffffff) != R615_MCTP_NVIDIA)
		return -EOPNOTSUPP;
	if (outer.total_bytes > available)
		return -EAGAIN;
	message.type = outer.nvdm >> 24;
	if (message.type == R615_MESSAGE_RPC) {
		struct r615_message_rpc rpc;

		header_bytes = sizeof(rpc);
		if (outer.payload_bytes < header_bytes)
			return -EPROTO;
		memcpy(&rpc, (const u8 *)buffer + sizeof(outer), sizeof(rpc));
		/* Major version 3 (minor may advance); length covers header+params. */
		if ((rpc.version & 0xff000000U) != R615_RPC_VERSION || rpc.signature != R615_RPC_SIGNATURE ||
		    rpc.bytes < header_bytes || rpc.bytes > outer.payload_bytes)
			return -EPROTO;
		message.function = rpc.function;
		message.sequence = rpc.sequence;
		message.status_or_limit = rpc.status;
		message.private_status = rpc.private_status;
		message.gfid = rpc.gfid;
		message.bytes = rpc.bytes - header_bytes;
	} else if (message.type == R615_MESSAGE_GMCAPI) {
		struct r615_message_gmc gmc;

		header_bytes = sizeof(gmc);
		if (outer.payload_bytes < header_bytes)
			return -EPROTO;
		memcpy(&gmc, (const u8 *)buffer + sizeof(outer), sizeof(gmc));
		/* GMCAPI_HEADER.size is authoritative (public gpuSendGmcRpc). */
		if (gmc.bytes > outer.payload_bytes - header_bytes ||
		    (gmc.command & 0xfe000000U) || (gmc.command & 0xffffff) <= 0xffff)
			return -EPROTO;
		message.function = gmc.command & 0xffffff;
		message.response = !!(gmc.command & R615_GMC_RESPONSE);
		message.sequence = gmc.sequence;
		message.status_or_limit = gmc.status_or_limit;
		message.bytes = gmc.bytes;
	} else {
		return -EOPNOTSUPP;
	}
	message.data = (const u8 *)buffer + sizeof(outer) + header_bytes;
	*output = message;
	return 0;
}

int
r615_message_send(struct r615_msgq *queue, const struct r615_message *message,
		  void *staging, size_t capacity)
{
	u32 bytes;
	int ret;

	if (!queue || !queue->memory || !queue->tx_started ||
	    r615_message_overlap(staging, capacity, queue->memory, R615_MSGQ_SHARED_BYTES) ||
	    r615_message_overlap(staging, capacity, queue, sizeof(*queue)))
		return -EINVAL;
	if (queue->failed)
		return -EIO;
	ret = r615_message_build(message, staging, capacity, &bytes);

	if (ret)
		return ret;
	return r615_msgq_send(queue, staging, bytes);
}

int
r615_message_receive(struct r615_msgq *queue, void *staging, size_t capacity,
		     struct r615_message *output)
{
	struct r615_message message;
	struct r615_message_outer outer;
	u32 bytes = R615_MSGQ_PAGE_BYTES;
	int ret;

	if (!queue || !queue->memory || !staging || !output || capacity < R615_MESSAGE_MAX_BYTES ||
	    r615_message_overlap(staging, capacity, output, sizeof(*output)) ||
	    r615_message_overlap(staging, capacity, queue, sizeof(*queue)) ||
	    r615_message_overlap(output, sizeof(*output), queue->memory, R615_MSGQ_SHARED_BYTES) ||
	    r615_message_overlap(output, sizeof(*output), queue, sizeof(*queue)))
		return -EINVAL;
	ret = r615_msgq_peek(queue, staging, bytes);
	if (ret)
		return ret;
	ret = r615_message_parse(staging, bytes, &message);
	if (ret == -EAGAIN) {
		/* parse validated the envelope and bounded its total before returning EAGAIN. */
		memcpy(&outer, staging, sizeof(outer));
		bytes = (outer.total_bytes + R615_MSGQ_PAGE_BYTES - 1) &
			~(R615_MSGQ_PAGE_BYTES - 1);
		ret = r615_msgq_peek(queue, staging, bytes);
		if (ret)
			return ret;
		ret = r615_message_parse(staging, bytes, &message);
	}
	if (ret)
		return ret;
	ret = r615_msgq_consume_frame(queue, bytes / R615_MSGQ_PAGE_BYTES);
	if (ret)
		return ret;
	*output = message;
	return 0;
}
