// SPDX-License-Identifier: MIT
/* Retained, firmware-written SR radix3 pages and independent coherent metadata.
 * This resource does not implement FBSR or authorize device suspend on its own.
 */
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "dma_image.h"
#include "sr.h"

struct r615_sr {
	struct device *device;
	struct r615_dma_image *image;
	struct r615_sr_meta *meta;
	dma_addr_t address;
	bool exposed, idle, quarantined;
};

int
r615_sr_new(struct device *device, const struct r615_wpr_meta *wpr, struct r615_sr **output)
{
	struct r615_sr *sr;
	u64 bytes, root, allocated;
	int ret;

	if (!device || !output || *output)
		return -EINVAL;
	ret = r615_sr_size(wpr, &bytes);
	if (ret)
		return ret;
	sr = kzalloc_obj(*sr, GFP_KERNEL);
	if (!sr)
		return -ENOMEM;
	sr->device = get_device(device);
	ret = r615_dma_image_zero_new(device, bytes, &sr->image);
	if (ret)
		goto fail;
	sr->meta = dma_alloc_coherent(device, R615_BOOT_PAGE, &sr->address, GFP_KERNEL);
	if (!sr->meta) {
		ret = -ENOMEM;
		goto fail;
	}
	memset(sr->meta, 0, R615_BOOT_PAGE);
	if (!sr->address || sr->address % R615_BOOT_PAGE ||
	    sr->address > (1ULL << 49) - R615_BOOT_PAGE) {
		ret = -EINVAL;
		goto fail;
	}
	/* Internal exposure remains private to the SR owner until publish. */
	ret = r615_dma_image_expose(sr->image, &root, &allocated);
	if (ret)
		goto fail;
	ret = r615_sr_meta_build(wpr, root, allocated, sr->meta);
	if (ret)
		goto fail;
	*output = sr;
	return 0;
fail:
	r615_sr_del(&sr);
	return ret;
}

int
r615_sr_publish(struct r615_sr *sr, u64 *metadata_address)
{
	if (!sr || !metadata_address)
		return -EINVAL;
	if (sr->quarantined)
		return -ESHUTDOWN;
	if (sr->exposed && !sr->idle)
		return -EALREADY;
	/* Do not rewrite internal[], subrevision or data pages on resume. */
	dma_wmb();
	sr->exposed = true;
	sr->idle = false;
	*metadata_address = sr->address;
	return 0;
}

void
r615_sr_quiesced(struct r615_sr *sr)
{
	if (sr)
		sr->idle = true;
}

void
r615_sr_quarantine(struct r615_sr *sr)
{
	if (sr)
		sr->quarantined = true;
}

bool
r615_sr_dma_owned(const struct r615_sr *sr, u64 address, u64 bytes)
{
	if (!sr || !sr->exposed || sr->idle || sr->quarantined || !bytes || address > ~0ULL - bytes)
		return false;
	if (address >= sr->address && address - sr->address < R615_BOOT_PAGE &&
	    bytes <= R615_BOOT_PAGE - (address - sr->address))
		return true;
	return r615_dma_image_owns(sr->image, address, bytes);
}

int
r615_sr_del(struct r615_sr **output)
{
	struct r615_sr *sr;

	if (!output || !*output)
		return 0;
	sr = *output;
	if (sr->quarantined || (sr->exposed && !sr->idle))
		return -EBUSY;
	/* No consumer ever received this resource, or the owner proved quiescence. */
	r615_dma_image_quiesced(sr->image);
	r615_dma_image_del(&sr->image);
	if (sr->meta)
		dma_free_coherent(sr->device, R615_BOOT_PAGE, sr->meta, sr->address);
	put_device(sr->device);
	kfree(sr);
	*output = NULL;
	return 0;
}
