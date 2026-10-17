// SPDX-License-Identifier: MIT
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "dma_image.h"

/* Public 615 image/bindata sizes fit well within this safety bound. */
#define R615_DMA_IMAGE_MAX_BYTES (128U << 20)

struct r615_dma_image {
	struct device *device;
	struct r615_radix3_plan plan;
	struct r615_dma_page {
		void *cpu;
		dma_addr_t address;
	} *pages;
	u32 allocated_pages;
	void *tables;
	dma_addr_t table_address;
	bool exposed, stopped;
};

static int
r615_dma_image_alloc(struct device *device, const void *data, size_t bytes,
		     struct r615_dma_image **output)
{
	struct r615_dma_image *image;
	u64 *addresses = NULL;
	int ret;

	if (!device || !bytes || !output || *output)
		return -EINVAL;
	image = kzalloc_obj(*image, GFP_KERNEL);
	if (!image)
		return -ENOMEM;
	image->device = get_device(device);
	ret = r615_radix3_plan(bytes, &image->plan);
	if (ret)
		goto fail;
	image->pages = kvcalloc(image->plan.data_pages, sizeof(*image->pages), GFP_KERNEL);
	addresses = kvcalloc(image->plan.data_pages, sizeof(*addresses), GFP_KERNEL);
	if (!image->pages || !addresses) {
		ret = -ENOMEM;
		goto fail;
	}
	image->tables = dma_alloc_coherent(device, image->plan.table_bytes,
					   &image->table_address, GFP_KERNEL);
	if (!image->tables) {
		ret = -ENOMEM;
		goto fail;
	}
	for (u32 i = 0; i < image->plan.data_pages; i++) {
		struct r615_dma_page *page = &image->pages[i];
		size_t offset = (size_t)i * R615_BOOT_PAGE;
		size_t count = bytes - offset;

		if (count > R615_BOOT_PAGE)
			count = R615_BOOT_PAGE;
		page->cpu = dma_alloc_coherent(device, R615_BOOT_PAGE, &page->address, GFP_KERNEL);
		if (!page->cpu) {
			ret = -ENOMEM;
			goto fail;
		}
		image->allocated_pages++;
		memset(page->cpu, 0, R615_BOOT_PAGE);
		if (data)
			memcpy(page->cpu, (const u8 *)data + offset, count);
		addresses[i] = page->address;
	}
	ret = r615_radix3_build(&image->plan, image->tables, image->plan.table_bytes,
			      image->table_address, addresses, image->plan.data_pages);
	if (ret)
		goto fail;
	dma_wmb();
	kvfree(addresses);
	*output = image;
	return 0;
fail:
	kvfree(addresses);
	r615_dma_image_del(&image);
	return ret;
}

int
r615_dma_image_new(struct device *device, const void *data, size_t bytes,
		   struct r615_dma_image **output)
{
	if (!data)
		return -EINVAL;
	if (bytes > R615_DMA_IMAGE_MAX_BYTES)
		return -E2BIG;
	return r615_dma_image_alloc(device, data, bytes, output);
}

int
r615_dma_image_zero_new(struct device *device, size_t bytes, struct r615_dma_image **output)
{
	/* GA107's matched WPR producer never exceeds its 256 MiB pre-scrub area.
	 * Do not widen firmware-image limits just to accommodate an SR backup.
	 */
	if (!bytes || bytes % R615_BOOT_PAGE)
		return -EINVAL;
	if (bytes > (256U << 20))
		return -E2BIG;
	return r615_dma_image_alloc(device, NULL, bytes, output);
}

int
r615_dma_image_copy(const struct r615_dma_image *image, u64 offset, void *output, u32 bytes)
{
	u8 *target = output;

	if (!image || !output || offset > image->plan.data_bytes || bytes > image->plan.data_bytes - offset)
		return -EINVAL;
	while (bytes) {
		u32 page = offset / R615_BOOT_PAGE, within = offset % R615_BOOT_PAGE;
		u32 count = R615_BOOT_PAGE - within;

		if (count > bytes)
			count = bytes;
		memcpy(target, (u8 *)image->pages[page].cpu + within, count);
		target += count;
		offset += count;
		bytes -= count;
	}
	return 0;
}

int
r615_dma_image_pages(const struct r615_dma_image *image, u64 *addresses, u32 count)
{
	if (!image || !addresses || count != image->allocated_pages)
		return -EINVAL;
	if (image->stopped)
		return -ESHUTDOWN;
	for (u32 i = 0; i < count; i++)
		addresses[i] = image->pages[i].address;
	return 0;
}

int
r615_dma_image_expose(struct r615_dma_image *image, u64 *root, u64 *bytes)
{
	if (!image || !root || !bytes || root == bytes)
		return -EINVAL;
	if (image->stopped)
		return -ESHUTDOWN;
	if (image->exposed)
		return -EALREADY;
	image->exposed = true;
	*root = image->table_address;
	*bytes = image->plan.data_bytes;
	return 0;
}

void
r615_dma_image_quiesced(struct r615_dma_image *image)
{
	if (image)
		image->stopped = true;
}

bool
r615_dma_image_owns(const struct r615_dma_image *image, u64 address, u64 bytes)
{
	u64 end;

	if (!image || !image->exposed || image->stopped || !bytes || address > ~0ULL - bytes)
		return false;
	end = address + bytes;
	if (address >= image->table_address && address - image->table_address <= image->plan.table_bytes &&
	    bytes <= image->plan.table_bytes - (address - image->table_address))
		return true;
	/* The request is a physical interval, not a walk of the radix tree. Never
	 * treat a gap between separately allocated coherent pages as owned DMA.
	 */
	while (address < end) {
		u64 next = address;

		for (u32 i = 0; i < image->allocated_pages; i++) {
			u64 base = image->pages[i].address;

			if (address >= base && address - base < R615_BOOT_PAGE) {
				next = base + R615_BOOT_PAGE;
				break;
			}
		}
		if (next == address)
			return false;
		address = next;
	}
	return true;
}

int
r615_dma_image_del(struct r615_dma_image **output)
{
	struct r615_dma_image *image;

	if (!output || !*output)
		return 0;
	image = *output;
	if (image->exposed && !image->stopped)
		return -EBUSY;
	for (u32 i = 0; i < image->allocated_pages; i++)
		dma_free_coherent(image->device, R615_BOOT_PAGE,
				  image->pages[i].cpu, image->pages[i].address);
	if (image->tables)
		dma_free_coherent(image->device, image->plan.table_bytes,
				  image->tables, image->table_address);
	kvfree(image->pages);
	put_device(image->device);
	kfree(image);
	*output = NULL;
	return 0;
}
