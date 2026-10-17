/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_FIRMWARE_H__
#define __NVKM_R615_FIRMWARE_H__

#include <linux/types.h>

struct r615_firmware_view {
	const void *image, *signature;
	size_t image_bytes, signature_bytes;
};

struct r615_bootloader_view {
	const void *image;
	u32 image_bytes, code_offset, data_offset, manifest_offset, app_version;
};

/* Borrow validated spans from matched, caller-owned firmware; execute nothing. */
int r615_firmware_parse(const void *data, size_t bytes, struct r615_firmware_view *output);
int r615_bootloader_parse(const void *data, size_t bytes, struct r615_bootloader_view *output);

#endif
