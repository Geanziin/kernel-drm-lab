// SPDX-License-Identifier: MIT
/* Implementation from the public MIT 615 host ABI/register contract only.
 * No firmware body is embedded here. BROM/bootloader authentication is kept.
 */
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "host_exec.h"

static_assert(sizeof(struct r615_bl_dmem) == 84);
static_assert(sizeof(struct r615_generic_request) == 112);
static_assert(sizeof(struct r615_hs_request) == 80);

static bool
r615_host_range(u32 offset, u32 bytes, u32 limit)
{
	return offset <= limit && bytes <= limit - offset;
}

static u32
r615_host_dma_bytes(u32 bytes)
{
	return bytes > 0xffffff00U ? 0 : (bytes + 255U) & ~255U;
}

int
r615_loader_parse(const void *data, size_t bytes, struct r615_loader_view *out)
{
	u32 desc[6];
	u64 code_end, data_end, image_bytes;

	if (!data || !out || bytes < sizeof(desc) || bytes > (16U << 20))
		return -EINVAL;
	memcpy(desc, data, sizeof(desc));
	code_end = (u64)desc[2] + desc[3];
	data_end = (u64)desc[4] + desc[5];
	image_bytes = code_end > data_end ? code_end : data_end;
	if (desc[1] || !desc[3] || desc[2] % 4 || desc[3] % 256 ||
	    desc[0] > 0xffff || desc[3] / 256 > 0x10000U - desc[0] ||
	    image_bytes != bytes - sizeof(desc))
		return -EPROTO;
	*out = (struct r615_loader_view) {
		.code = (const u8 *)data + sizeof(desc) + desc[2],
		.code_bytes = desc[3], .start_tag = desc[0],
	};
	return 0;
}

static int
r615_host_source(const struct r615_host_ops *ops, void *ctx, u32 space,
		 u64 base, u32 offset, u32 bytes)
{
	u32 dma_bytes = r615_host_dma_bytes(bytes);

	if (!bytes)
		return 0;
	if (!dma_bytes || base % 256 || offset % 256 ||
	    base > ~0ULL - offset || base + offset > ~0ULL - dma_bytes ||
	    base + offset + dma_bytes > (1ULL << 49))
		return -EINVAL;
	return ops->memory(ctx, space, base + offset, dma_bytes);
}

static int
r615_host_generic_validate(const struct r615_host_config *config,
			   const struct r615_host_ops *ops, void *ctx,
			   const struct r615_generic_request *request)
{
	const struct r615_bl_dmem *dmem = &request->dmem;
	u32 code_limit;
	u64 code = (u64)dmem->code.hi << 32 | dmem->code.lo;
	u64 data = (u64)dmem->data.hi << 32 | dmem->data.lo;
	bool entry_nonsecure, entry_secure;
	int ret;

	if (!config->loader.code || !config->loader.code_bytes ||
	    config->loader.code_bytes > config->imem_bytes || config->loader.code_bytes % 256 ||
	    config->loader.start_tag > 0xffff ||
	    config->loader.code_bytes / 256 > 0x10000U - config->loader.start_tag)
		return -ENOENT;
	code_limit = config->imem_bytes - config->loader.code_bytes;
	if (request->descriptor_bytes != sizeof(*dmem) || dmem->dma_index >= 8 ||
	    (request->address_space != R615_HOST_SYSMEM && request->address_space != R615_HOST_FBMEM) ||
	    request->cache_attribute > 1 || sizeof(*dmem) > config->dmem_bytes ||
	    !r615_host_range(dmem->nonsecure_offset, r615_host_dma_bytes(dmem->nonsecure_bytes), code_limit) ||
	    !r615_host_range(dmem->secure_offset, r615_host_dma_bytes(dmem->secure_bytes), code_limit) ||
	    dmem->data_bytes > config->dmem_bytes ||
	    r615_host_dma_bytes(dmem->data_bytes) > config->dmem_bytes)
		return -EINVAL;
	for (u32 i = 0; i < 4; i++) {
		if (dmem->reserved[i] || request->reserved[i])
			return -EPROTO;
	}
	entry_nonsecure = dmem->entry >= dmem->nonsecure_offset &&
			  dmem->entry - dmem->nonsecure_offset < dmem->nonsecure_bytes;
	entry_secure = dmem->entry >= dmem->secure_offset &&
		       dmem->entry - dmem->secure_offset < dmem->secure_bytes;
	if (!entry_nonsecure && !entry_secure)
		return -EINVAL;
	ret = r615_host_source(ops, ctx, request->address_space, code,
			       dmem->nonsecure_offset, dmem->nonsecure_bytes);
	if (!ret)
		ret = r615_host_source(ops, ctx, request->address_space, code,
				       dmem->secure_offset, dmem->secure_bytes);
	if (!ret)
		ret = r615_host_source(ops, ctx, request->address_space, data, 0, dmem->data_bytes);
	return ret;
}

static int
r615_host_hs_validate(const struct r615_host_config *config,
		      const struct r615_host_ops *ops, void *ctx,
		      const struct r615_hs_request *request)
{
	u32 dmem_offset = request->dmem_va == ~0U ? 0 : request->dmem_va;
	int ret;

	if (!request->imem_bytes || !request->dmem_bytes ||
	    request->imem_pa % 256 || request->dmem_pa % 256 ||
	    !r615_host_range(request->imem_pa, request->imem_bytes, config->imem_bytes) ||
	    !r615_host_range(request->dmem_pa, request->dmem_bytes, config->dmem_bytes) ||
	    !r615_host_range(request->imem_va, request->imem_bytes, 1U << 24) ||
	    !r615_host_range(dmem_offset, request->dmem_bytes, ~0U) ||
	    request->signature_dmem % 4 || request->signature_dmem < request->dmem_pa ||
	    request->signature_dmem - request->dmem_pa >= request->dmem_bytes ||
	    !request->engine_mask || request->ucode_id > 255)
		return -EINVAL;
	for (u32 i = 0; i < 2; i++) {
		if (request->reserved64[i])
			return -EPROTO;
	}
	for (u32 i = 0; i < 3; i++) {
		if (request->reserved32[i])
			return -EPROTO;
	}
	ret = r615_host_source(ops, ctx, R615_HOST_FBMEM, request->imem_address,
			       request->imem_va, request->imem_bytes);
	if (!ret)
		ret = r615_host_source(ops, ctx, R615_HOST_FBMEM, request->dmem_address,
				       dmem_offset, request->dmem_bytes);
	return ret;
}

static int
r615_host_start(const struct r615_host_ops *ops, void *ctx, u32 entry)
{
	u32 control;
	int ret = ops->write(ctx, 0x040, 0xfe);

	if (!ret)
		ret = ops->write(ctx, 0x104, entry);
	if (!ret)
		ret = ops->read(ctx, 0x100, &control);
	if (!ret)
		ret = ops->write(ctx, control & 0x40 ? 0x130 : 0x100, 2);
	if (!ret)
		ret = ops->poll(ctx, 0x100, 0x10, 0x10);
	return ret;
}

static int
r615_host_pio(const struct r615_host_ops *ops, void *ctx, bool imem,
	       u32 destination, const void *data, u32 bytes, u32 tag)
{
	const u8 *source = data;
	int ret = ops->write(ctx, imem ? 0x180 : 0x1c0, 0x01000000U | destination);

	for (u32 i = 0; !ret && i < bytes; i += 4) {
		u32 value;

		if (imem && !(i % 256))
			ret = ops->write(ctx, 0x188, tag + i / 256);
		if (ret)
			break;
		memcpy(&value, source + i, sizeof(value));
		ret = ops->write(ctx, imem ? 0x184 : 0x1c4, value);
	}
	return ret;
}

static int
r615_host_generic(const struct r615_host_config *config, const struct r615_host_ops *ops,
		  void *ctx, const struct r615_generic_request *request)
{
	u32 reg = 0x600 + request->dmem.dma_index * 4, saved, target = 0;
	int ret = ops->read(ctx, reg, &saved);

	if (ret)
		return ret;
	if (request->address_space == R615_HOST_SYSMEM)
		target = request->cache_attribute ? 2 : 1;
	ret = ops->write(ctx, reg, (saved & ~7U) | target | 4);
	if (!ret)
		ret = r615_host_pio(ops, ctx, false, 0, &request->dmem, sizeof(request->dmem), 0);
	if (!ret)
		ret = r615_host_pio(ops, ctx, true, config->imem_bytes - config->loader.code_bytes,
				    config->loader.code, config->loader.code_bytes, config->loader.start_tag);
	if (!ret)
		ret = r615_host_start(ops, ctx, config->loader.start_tag << 8);
	/* A failed start/poll may leave live DMA. Do not change aperture then. */
	if (ret)
		return ret;
	ret = ops->write(ctx, reg, saved);
	return ret ? ret : ops->resume(ctx);
}

static int
r615_host_dma(const struct r615_host_ops *ops, void *ctx, u64 address, u32 offset,
	       u32 destination, u32 bytes, u32 command)
{
	int ret = ops->poll(ctx, 0x118, 1, 0);

	if (!ret)
		ret = ops->write(ctx, 0x110, (u32)(address >> 8));
	if (!ret)
		ret = ops->write(ctx, 0x128, (u32)(address >> 40) & 0x1ff);
	for (u32 i = 0; !ret && i < bytes; i += 256) {
		ret = ops->poll(ctx, 0x118, 1, 0);
		if (!ret)
			ret = ops->write(ctx, 0x114, destination + i);
		if (!ret)
			ret = ops->write(ctx, 0x11c, offset + i);
		if (!ret)
			ret = ops->write(ctx, 0x118, command);
	}
	if (!ret)
		ret = ops->poll(ctx, 0x118, 2, 2);
	return ret;
}

static int
r615_host_hs(const struct r615_host_ops *ops, void *ctx, const struct r615_hs_request *request)
{
	u32 config;
	int ret = ops->read(ctx, 0x600, &config);

	if (!ret)
		ret = ops->write(ctx, 0x600, (config & ~0x10007U) | 4);
	if (!ret)
		ret = r615_host_dma(ops, ctx, request->imem_address, request->imem_va,
				    request->imem_pa, request->imem_bytes, 0x614);
	if (!ret)
		ret = r615_host_dma(ops, ctx, request->dmem_address,
				    request->dmem_va == ~0U ? 0 : request->dmem_va,
				    request->dmem_pa, request->dmem_bytes,
				    0x600 | (request->dmem_va == ~0U ? 0 : 0x10000));
	if (!ret)
		ret = ops->write(ctx, 0x1210, request->signature_dmem);
	if (!ret)
		ret = ops->write(ctx, 0x119c, request->engine_mask);
	if (!ret)
		ret = ops->write(ctx, 0x1198, request->ucode_id);
	if (!ret)
		ret = ops->write(ctx, 0x1180, 1); /* RSA3K, no authentication bypass. */
	if (!ret)
		ret = r615_host_start(ops, ctx, request->imem_va);
	return ret ? ret : ops->resume(ctx);
}

int
r615_host_execute(const struct r615_host_config *config, const struct r615_host_ops *ops,
		  void *ctx, u32 command, const void *data, u32 bytes)
{
	union { struct r615_generic_request generic; struct r615_hs_request hs; } request;
	u32 dma_control;
	int ret;

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	return -EOPNOTSUPP;
#endif
	if (!config || !ops || !data || !ops->read || !ops->write || !ops->poll ||
	    !ops->reset || !ops->resume || !ops->memory || !config->imem_bytes ||
	    config->imem_bytes % 256 || !config->dmem_bytes || config->dmem_bytes % 256)
		return -EINVAL;
	if (config->imem_bytes > (1U << 24) || config->dmem_bytes > (1U << 24))
		return -EINVAL;
	if (command == R615_HOST_GENERIC && bytes == sizeof(request.generic)) {
		memcpy(&request.generic, data, bytes);
		ret = r615_host_generic_validate(config, ops, ctx, &request.generic);
	} else if (command == R615_HOST_HS && bytes == sizeof(request.hs)) {
		memcpy(&request.hs, data, bytes);
		/* The public GA102 transfer loop emits whole 256-byte blocks even
		 * when a descriptor gives an unrounded length. Preflight the padding.
		 */
		request.hs.imem_bytes = r615_host_dma_bytes(request.hs.imem_bytes);
		request.hs.dmem_bytes = r615_host_dma_bytes(request.hs.dmem_bytes);
		ret = r615_host_hs_validate(config, ops, ctx, &request.hs);
	} else {
		return -EOPNOTSUPP;
	}
	if (ret)
		return ret;
	ret = ops->poll(ctx, 0x040, 0x80000000U, 0x80000000U);
	if (!ret)
		ret = ops->reset(ctx);
	/* Public kflcnDisableCtxReq_TU102 (GA107 HAL): FBIF_CTL (FBIF base 0x600
	 * + 0x24) ALLOW_PHYS_NO_CTX bit 7, then DMACTL = 0. Clearing only
	 * REQUIRE_CTX leaves physical DMA without a bound context stuck (boot 11:
	 * HS IMEM transfer timed out).
	 */
	if (!ret)
		ret = ops->read(ctx, 0x624, &dma_control);
	if (!ret)
		ret = ops->write(ctx, 0x624, dma_control | 0x80);
	if (!ret)
		ret = ops->write(ctx, 0x10c, 0);
	if (ret)
		return ret;
	return command == R615_HOST_GENERIC ? r615_host_generic(config, ops, ctx, &request.generic) :
		r615_host_hs(ops, ctx, &request.hs);
}
