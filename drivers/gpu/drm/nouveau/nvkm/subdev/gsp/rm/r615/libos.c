// SPDX-License-Identifier: MIT
#include <linux/errno.h>
#include <linux/build_bug.h>
#include <linux/string.h>
#include "libos.h"

static_assert(sizeof(struct r615_libos_event) == 8);

int
r615_libos_validate(const void *data, u32 bytes)
{
	struct r615_libos_event header;

	if (!data || bytes < sizeof(header))
		return -EPROTO;
	memcpy(&header, data, sizeof(header));
	if (header.bytes != bytes - sizeof(header) || header.bytes < 2 * sizeof(u64) ||
	    header.bytes > R615_LIBOS_MAX_BYTES || header.bytes % sizeof(u64))
		return -EPROTO;
	return header.engine == R615_LIBOS_PMU_ENGINE ? 0 : -EOPNOTSUPP;
}

int
r615_libos_update(struct r615_libos_capture *capture, const void *data, u32 bytes)
{
	const u8 *buffer;
	int ret;

	if (!capture)
		return -EINVAL;
	ret = r615_libos_validate(data, bytes);
	if (ret)
		return ret;
	buffer = (const u8 *)data + sizeof(struct r615_libos_event);
	bytes -= sizeof(struct r615_libos_event);
	memcpy(capture->data, buffer, bytes);
	memset(capture->data + bytes, 0, sizeof(capture->data) - bytes);
	memcpy(&capture->put, buffer, sizeof(capture->put));
	capture->bytes = bytes;
	if (capture->events != ~0ULL)
		capture->events++;
	return 0;
}

int
r615_libos_copy(const struct r615_libos_capture *capture, void *output, u32 limit, u32 *bytes)
{
	if (!capture || !output || !bytes)
		return -EINVAL;
	if (!capture->events)
		return -ENOENT;
	if (limit < capture->bytes)
		return -ENOSPC;
	memcpy(output, capture->data, capture->bytes);
	*bytes = capture->bytes;
	return 0;
}
