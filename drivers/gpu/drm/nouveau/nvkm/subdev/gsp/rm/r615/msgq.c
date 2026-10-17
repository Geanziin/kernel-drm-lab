// SPDX-License-Identifier: MIT
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "msgq.h"

static_assert(sizeof(struct r615_msgq_header) == 32, "MSGQ 2.0 header");

#define R615_MSGQ_SLOTS (R615_MSGQ_QUEUE_BYTES / R615_MSGQ_PAGE_BYTES - 1U)

static const struct r615_msgq_header r615_msgq_header = {
	.major = 2,
	.bytes = R615_MSGQ_QUEUE_BYTES,
	.slot_bytes = R615_MSGQ_PAGE_BYTES,
	.slots = R615_MSGQ_SLOTS,
	.entry_offset = R615_MSGQ_PAGE_BYTES,
};

static bool
r615_msgq_outside(const void *object, size_t bytes, const void *memory)
{
	unsigned long start = (unsigned long)object;
	unsigned long base = (unsigned long)memory;

	return start <= ~0UL - bytes &&
	       (start >= base + R615_MSGQ_SHARED_BYTES || start + bytes <= base);
}

int
r615_msgq_prepare(struct r615_msgq *queue, const char *version, void *memory,
		  size_t bytes, u64 dma_address, const struct r615_msgq_ops *ops,
		  void *context, struct r615_init_args *output)
{
	struct r615_boot_config config = {
		.queue = {
			.shared_mem_pa = dma_address,
			.cmd_offset = R615_MSGQ_PAGE_BYTES,
			.status_offset = R615_MSGQ_PAGE_BYTES + R615_MSGQ_QUEUE_BYTES,
			.element_header_bytes = R615_MSGQ_ELEMENT_HEADER_BYTES,
			.element_min_bytes = R615_MSGQ_PAGE_BYTES,
			.element_max_bytes = R615_MSGQ_MAX_SLOTS * R615_MSGQ_PAGE_BYTES,
			.header_alignment_log2 = 4,
			.element_alignment_log2 = 12,
			.pte_count = R615_MSGQ_PTE_COUNT,
		},
		.shared_mem_bytes = bytes,
		/* GA107 cold boot, pagingConfig == 0: public 615 default stack policy. */
		.dmem_stack = true,
	};
	struct r615_init_args args;
	u8 *data = memory;
	int ret;

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	return -EOPNOTSUPP;
#endif
	if (!queue || !output || !memory || !ops || !ops->read || !ops->write ||
	    !ops->acquire || !ops->publish || queue->memory)
		return -EINVAL;
	if (!version || strcmp(version, "615.78.08"))
		return -EOPNOTSUPP;
	if (bytes != R615_MSGQ_SHARED_BYTES ||
	    (unsigned long)memory % R615_MSGQ_PAGE_BYTES || !dma_address ||
	    dma_address % R615_MSGQ_PAGE_BYTES || dma_address > ~0ULL - bytes ||
	    (unsigned long)memory > ~0UL - bytes)
		return -EINVAL;
	if (!r615_msgq_outside(queue, sizeof(*queue), memory) ||
	    !r615_msgq_outside(output, sizeof(*output), memory) ||
	    !r615_msgq_outside(ops, sizeof(*ops), memory))
		return -EINVAL;
	ret = r615_build_init_args(&config, &args);
	if (ret)
		return ret;

	memset(memory, 0, bytes);
	for (u32 i = 0; i < R615_MSGQ_PTE_COUNT; i++) {
		u64 address = dma_address + (u64)i * R615_MSGQ_PAGE_BYTES;

		memcpy(data + i * sizeof(address), &address, sizeof(address));
	}
	memcpy(data + R615_MSGQ_PAGE_BYTES, &r615_msgq_header, sizeof(r615_msgq_header));
	*queue = (struct r615_msgq) { .memory = memory, .ops = ops, .context = context };
	*output = args;
	return 0;
}

static int
r615_msgq_ready(struct r615_msgq *queue, bool receive)
{
	if (!queue || !queue->memory || !queue->tx_started)
		return -EINVAL;
	if (queue->failed)
		return -EIO;
	if (receive && !queue->rx_linked)
		return -EAGAIN;
	return 0;
}

static int
r615_msgq_publish(struct r615_msgq *queue, enum r615_msgq_register reg, u32 value)
{
	int ret;

	queue->ops->publish(queue->context);
	ret = queue->ops->write(queue->context, reg, value);
	/* A failed MMIO operation may already have reached the peer. Never retry it. */
	if (ret)
		queue->failed = true;
	return ret;
}

int
r615_msgq_start(struct r615_msgq *queue)
{
	if (!queue || !queue->memory || queue->tx_started)
		return -EINVAL;
	if (queue->failed)
		return -EIO;
	queue->tx_started = true;
	return r615_msgq_publish(queue, R615_MSGQ_TX_HEAD, 0);
}

int
r615_msgq_link_rx(struct r615_msgq *queue)
{
	struct r615_msgq_header header;
	struct r615_msgq_header empty = { 0 };
	u32 head;
	int ret = r615_msgq_ready(queue, false);

	if (ret)
		return ret;
	if (queue->rx_linked)
		return -EALREADY;
	ret = queue->ops->read(queue->context, R615_MSGQ_RX_HEAD, &head);
	if (ret)
		return ret;
	queue->ops->acquire(queue->context);
	memcpy(&header, (u8 *)queue->memory + R615_MSGQ_PAGE_BYTES + R615_MSGQ_QUEUE_BYTES,
	       sizeof(header));
	if (!memcmp(&header, &empty, sizeof(header)))
		return -EAGAIN;
	/* Minor revisions are compatible; unknown reserved fields are not consumed. */
	header.minor = 0;
	if (memcmp(&header, &r615_msgq_header, sizeof(header)))
		return -EPROTO;
	if (head > R615_MSGQ_SLOTS) {
		queue->failed = true;
		return -EPROTO;
	}
	queue->rx_linked = true;
	queue->rx_peer = head;
	return r615_msgq_publish(queue, R615_MSGQ_RX_TAIL, 0);
}

static int
r615_msgq_slot_count(struct r615_msgq *queue, const void *buffer, size_t bytes, u32 *slots)
{
	unsigned long start = (unsigned long)buffer;
	unsigned long base = (unsigned long)queue->memory;

	if (!buffer || !bytes || bytes % R615_MSGQ_PAGE_BYTES ||
	    bytes > R615_MSGQ_MAX_SLOTS * R615_MSGQ_PAGE_BYTES || start > ~0UL - bytes)
		return -EINVAL;
	if (start < base + R615_MSGQ_SHARED_BYTES && base < start + bytes)
		return -EINVAL;
	*slots = bytes / R615_MSGQ_PAGE_BYTES;
	return 0;
}

static int
r615_msgq_available(struct r615_msgq *queue, bool receive, u32 *available)
{
	u32 peer, ours, cached, pending;
	int ret;

	ret = queue->ops->read(queue->context, receive ? R615_MSGQ_RX_HEAD : R615_MSGQ_TX_TAIL,
			       &peer);
	if (ret)
		return ret;
	queue->ops->acquire(queue->context);
	ours = receive ? queue->rx_counter : queue->tx_counter;
	cached = receive ? queue->rx_peer : queue->tx_peer;
	pending = receive ? peer - ours : ours - peer;
	/* Public 615 msgqRxGetReadAvailable/msgqTxGetFreeSpace: the peer register
	 * can read back 0 after a Falcon reset (here: the HS-binary execution the
	 * GSP requests during GSP_INIT, boot 13). An impossible value falls back
	 * to the last plausible one; a reset of a full ring means empty. The
	 * cached value only under-reports, so no slot is read before written.
	 */
	if (pending > R615_MSGQ_SLOTS) {
		u32 cached_pending = receive ? cached - ours : ours - cached;

		if (cached_pending > R615_MSGQ_SLOTS) {
			queue->failed = true;
			return -EOVERFLOW;
		}
		if (peer == 0 && cached_pending == R615_MSGQ_SLOTS)
			peer = ours;
		else
			peer = cached;
		queue->resets++;
		pending = receive ? peer - ours : ours - peer;
	}
	if (receive)
		queue->rx_peer = peer;
	else
		queue->tx_peer = peer;
	*available = receive ? pending : R615_MSGQ_SLOTS - pending;
	return 0;
}

static void *
r615_msgq_slot(struct r615_msgq *queue, bool receive, u32 counter)
{
	u32 offset = 2U * R615_MSGQ_PAGE_BYTES;

	if (receive)
		offset += R615_MSGQ_QUEUE_BYTES;
	return (u8 *)queue->memory + offset + (counter % R615_MSGQ_SLOTS) * R615_MSGQ_PAGE_BYTES;
}

int
r615_msgq_send(struct r615_msgq *queue, const void *buffer, size_t bytes)
{
	u32 slots, available;
	int ret = r615_msgq_ready(queue, false);

	if (ret)
		return ret;
	ret = r615_msgq_slot_count(queue, buffer, bytes, &slots);
	if (ret)
		return ret;
	ret = r615_msgq_available(queue, false, &available);
	if (ret)
		return ret;
	if (slots > available)
		return -EAGAIN;
	for (u32 i = 0; i < slots; i++)
		memcpy(r615_msgq_slot(queue, false, queue->tx_counter + i),
		       (const u8 *)buffer + i * R615_MSGQ_PAGE_BYTES, R615_MSGQ_PAGE_BYTES);
	queue->tx_counter += slots;
	return r615_msgq_publish(queue, R615_MSGQ_TX_HEAD, queue->tx_counter);
}

int
r615_msgq_peek(struct r615_msgq *queue, void *buffer, size_t bytes)
{
	u32 slots, available;
	int ret = r615_msgq_ready(queue, true);

	if (ret)
		return ret;
	ret = r615_msgq_slot_count(queue, buffer, bytes, &slots);
	if (ret)
		return ret;
	ret = r615_msgq_available(queue, true, &available);
	if (ret)
		return ret;
	if (slots > available)
		return -EAGAIN;
	for (u32 i = 0; i < slots; i++)
		memcpy((u8 *)buffer + i * R615_MSGQ_PAGE_BYTES,
		       r615_msgq_slot(queue, true, queue->rx_counter + i), R615_MSGQ_PAGE_BYTES);
	return 0;
}

static int
r615_msgq_consume_slots(struct r615_msgq *queue, u32 slots, bool mark_seen)
{
	u32 available;
	int ret = r615_msgq_ready(queue, true);

	if (ret)
		return ret;
	if (!slots || slots > R615_MSGQ_MAX_SLOTS)
		return -EINVAL;
	ret = r615_msgq_available(queue, true, &available);
	if (ret)
		return ret;
	if (slots > available)
		return -EAGAIN;
	if (mark_seen) {
		u32 seen = 0x5345454e;

		memcpy(r615_msgq_slot(queue, true, queue->rx_counter), &seen, sizeof(seen));
	}
	queue->rx_counter += slots;
	return r615_msgq_publish(queue, R615_MSGQ_RX_TAIL, queue->rx_counter);
}

int
r615_msgq_consume(struct r615_msgq *queue, u32 slots)
{
	return r615_msgq_consume_slots(queue, slots, false);
}

int
r615_msgq_consume_frame(struct r615_msgq *queue, u32 slots)
{
	return r615_msgq_consume_slots(queue, slots, true);
}
