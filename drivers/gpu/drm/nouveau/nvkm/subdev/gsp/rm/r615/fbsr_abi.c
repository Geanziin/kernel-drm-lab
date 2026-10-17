// SPDX-License-Identifier: MIT
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "fbsr.h"

static_assert(sizeof(struct r615_memlist) == 56);
static_assert(sizeof(struct r615_fbsr_init) == 24);

int
r615_memlist_size(const struct r615_memlist_config *config, u32 *request_bytes)
{
	if (!config || !request_bytes || !config->pages || !config->client ||
	    !config->device || !config->memory || !config->bytes || config->bytes % 4096 ||
	    !config->page_count)
		return -EINVAL;
	/* Public pte_desc.length is 16 bits even though pageCount is 32 bits.
	 * No implicit truncation, no undocumented PDE/IDR scheme for larger lists.
	 */
	if (config->page_count > 0xffff)
		return -E2BIG;
	if (config->space == R615_MEMLIST_SYSMEM) {
		if (config->page_count != config->bytes / 4096)
			return -EINVAL;
	} else if (config->space == R615_MEMLIST_VRAM) {
		/* 615 rpcAllocMemory sends actual PTEs, one for a contiguous memdesc. */
		if (config->page_count != 1)
			return -EINVAL;
	} else {
		return -EOPNOTSUPP;
	}
	for (u32 i = 0; i < config->page_count; i++) {
		u64 bytes = config->space == R615_MEMLIST_VRAM ? config->bytes : 4096;

		if ((!config->pages[i] && config->space == R615_MEMLIST_SYSMEM) || config->pages[i] % 4096 ||
		    bytes > (1ULL << 49) || config->pages[i] > (1ULL << 49) - bytes)
			return -EINVAL;
	}
	*request_bytes = sizeof(struct r615_memlist) + config->page_count * sizeof(u64);
	return 0;
}

int
r615_memlist_build(const struct r615_memlist_config *config, void *output, u32 capacity)
{
	struct r615_memlist *request = output;
	u32 bytes;
	int ret = r615_memlist_size(config, &bytes);

	if (ret)
		return ret;
	if (!output || capacity < bytes)
		return -ENOSPC;
	memset(output, 0, bytes);
	request->client = config->client;
	request->device = config->device;
	request->memory = config->memory;
	request->class = config->space == R615_MEMLIST_SYSMEM ? 0x81 : 0x82;
	/* Public nvos.h: NO_MAP, NONCONTIGUOUS+PCI or CONTIGUOUS+VIDMEM;
	 * UNCACHED, as used by public mem_mgr_pwr_mgmt.c FBSR backup allocation.
	 */
	request->flags = 1U << 30 | (config->space == R615_MEMLIST_SYSMEM ? 1U << 4 : 2U << 8);
	request->format = config->space == R615_MEMLIST_VRAM ? 6 : 0;
	request->bytes = config->bytes;
	request->page_count = config->page_count;
	request->descriptor = config->page_count << 16; /* IDR=NONE, reserved=0. */
	for (u32 i = 0; i < config->page_count; i++)
		request->pfn[i] = config->pages[i] >> 12;
	return 0;
}
