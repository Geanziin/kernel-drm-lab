/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_DMA_IMAGE_H__
#define __NVKM_R615_DMA_IMAGE_H__

#include "bootmem.h"

struct device;
struct r615_dma_image;
/* Empty coherent radix3 storage for firmware-written SR data (page aligned). */
int r615_dma_image_zero_new(struct device *, size_t bytes, struct r615_dma_image **);
bool r615_dma_image_owns(const struct r615_dma_image *, u64 address, u64 bytes);
/* Private CPU copy for a flat FBSR PTE list; does not publish it to hardware. */
int r615_dma_image_pages(const struct r615_dma_image *, u64 *addresses, u32 count);
/* CPU read of privately owned data pages; caller supplies DMA ordering and
 * serialization against destruction. Does not expose a borrowed page pointer.
 */
int r615_dma_image_copy(const struct r615_dma_image *, u64 offset, void *, u32 bytes);

/*
 * Three-level DMA image for immutable boot data, including external bindata.
 * The boot owner serializes every operation and keeps the GSP owner alive.
 * A device reference protects the DMA allocation lifetime. No address is returned until
 * every page/table has been filled and published. expose() transfers lifetime
 * responsibility to the owner; del() then refuses to free until quiesced().
 */
int r615_dma_image_new(struct device *device, const void *data, size_t bytes,
		       struct r615_dma_image **output);
int r615_dma_image_expose(struct r615_dma_image *image, u64 *root, u64 *bytes);
/* Only after hardware DMA halt AND revocation of all boot-argument aliases. */
void r615_dma_image_quiesced(struct r615_dma_image *image);
int r615_dma_image_del(struct r615_dma_image **image);

#endif
