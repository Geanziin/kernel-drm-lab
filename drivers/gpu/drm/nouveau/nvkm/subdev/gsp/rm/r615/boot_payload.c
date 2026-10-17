// SPDX-License-Identifier: MIT
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <core/device.h>
#include <rm/rm.h>

#include "boot_payload.h"
#include "dma_image.h"
#include "firmware.h"

enum r615_boot_buffer_id {
	R615_BOOT_LOADER, R615_BOOT_SIGNATURE, R615_BOOT_WPR, R615_BOOT_LIBOS, R615_BOOT_ARGS,
	R615_BOOT_STATE_MONITOR,
	/* Baremetal log registration order, not the generic firmware task enum. */
	R615_BOOT_LOG_INIT, R615_BOOT_LOG_INTR, R615_BOOT_LOG_RM, R615_BOOT_LOG_MNOC,
	R615_BOOT_LOG_DEBUG, R615_BOOT_LOG_ROOT, R615_BOOT_LOG_RMON, R615_BOOT_LOG_KERNEL,
	R615_BOOT_BUFFER_COUNT,
};

/* Public 615 kernel_gsp.c:_kgspInitLibosLoggingStructures(), release defaults.
 * LOGINIT must be first. GA107 uses LibOS3 and enables LOGKRNL; baremetal
 * does not register the generic LOGVGPU task. Keep region and debugfs identity
 * in one table so the retained buffer cannot be mislabeled.
 */
static const struct {
	const char *id, *name;
	size_t bytes;
} r615_boot_logs[R615_BOOT_LOG_COUNT] = {
	{ "LOGINIT", "loginit", 0x10000 },
	{ "LOGINTR", "logintr", 0x10000 },
	{ "LOGRM",   "logrm",   0x10000 },
	{ "LOGMNOC", "logmnoc", 0x10000 },
	{ "LOGDBG",  "logdbg",  0x10000 },
	{ "LOGROOT", "logroot", 0x01000 },
	{ "LOGRMON", "logrmon", 0x01000 },
	{ "LOGKRNL", "logkrnl", 0x10000 },
};

struct r615_boot_payload {
	struct device *device;
	struct r615_transport *transport;
	struct r615_dma_image *image, *bindata;
	struct r615_boot_buffer {
		void *cpu;
		dma_addr_t address;
		size_t bytes;
	} buffers[R615_BOOT_BUFFER_COUNT];
	u64 bindata_address, bindata_bytes;
	u32 app_version;
	bool armed, host_disabled, pm_paused;
};

static int
r615_boot_buffer_new(struct r615_boot_payload *payload, enum r615_boot_buffer_id id,
		     size_t bytes)
{
	struct r615_boot_buffer *buffer = &payload->buffers[id];

	buffer->cpu = dma_alloc_coherent(payload->device, bytes, &buffer->address, GFP_KERNEL);
	if (!buffer->cpu)
		return -ENOMEM;
	buffer->bytes = bytes;
	memset(buffer->cpu, 0, bytes);
	if (!buffer->address || buffer->address % R615_BOOT_PAGE || buffer->address > ~0ULL - bytes)
		return -EINVAL;
	return 0;
}

int
r615_boot_payload_new(struct nvkm_gsp *gsp, const struct firmware *firmware,
		      const struct firmware *bootloader, const struct firmware *ucodes,
		      const struct r615_boot_environment *environment,
		      r615_transport_quiesce_func quiesce, void *context,
		      struct r615_boot_payload **output)
{
	struct r615_boot_payload *payload;
	struct r615_firmware_view fw;
	struct r615_bootloader_view boot;
	struct r615_libos_region regions[R615_BOOT_LOG_COUNT + 1] = { 0 };
	struct r615_wpr_config config = { 0 };
	u64 elf_address, elf_bytes;
	int ret;

	if (!gsp || !gsp->subdev.device || !gsp->subdev.device->dev || !firmware ||
	    !bootloader || !ucodes || !ucodes->data || !ucodes->size || !environment ||
	    !quiesce || !output || *output)
		return -EINVAL;
	if (gsp->subdev.device->chipset != 0x177 || !gsp->rm || !gsp->rm->version ||
	    strcmp(gsp->rm->version, "615.78.08"))
		return -EOPNOTSUPP;
	if (gsp->running)
		return -EBUSY;
	ret = r615_firmware_parse(firmware->data, firmware->size, &fw);
	if (ret)
		return ret;
	ret = r615_bootloader_parse(bootloader->data, bootloader->size, &boot);
	if (ret)
		return ret;
	payload = kzalloc_obj(*payload, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	payload->device = get_device(gsp->subdev.device->dev);
	payload->app_version = boot.app_version;
	ret = r615_transport_new(gsp, quiesce, context, &payload->transport);
	if (ret)
		goto fail;
	ret = r615_dma_image_new(payload->device, fw.image, fw.image_bytes, &payload->image);
	if (ret)
		goto fail;
	ret = r615_dma_image_new(payload->device, ucodes->data, ucodes->size, &payload->bindata);
	if (ret)
		goto fail;
	/* The roots remain private to this payload until arm; failure may still unwind. */
	ret = r615_dma_image_expose(payload->image, &elf_address, &elf_bytes);
	if (ret)
		goto fail;
	ret = r615_dma_image_expose(payload->bindata, &payload->bindata_address,
				    &payload->bindata_bytes);
	if (ret)
		goto fail;
	for (u32 i = 0; i < R615_BOOT_BUFFER_COUNT; i++) {
		size_t bytes = R615_BOOT_PAGE;

		if (i == R615_BOOT_LOADER)
			bytes = (boot.image_bytes + R615_BOOT_PAGE - 1) & ~(R615_BOOT_PAGE - 1);
		else if (i == R615_BOOT_STATE_MONITOR)
			bytes = R615_STATE_MONITOR_BYTES;
		else if (i >= R615_BOOT_LOG_INIT)
			bytes = r615_boot_logs[i - R615_BOOT_LOG_INIT].bytes;
		ret = r615_boot_buffer_new(payload, i, bytes);
		if (ret)
			goto fail;
	}
	memcpy(payload->buffers[R615_BOOT_LOADER].cpu, boot.image, boot.image_bytes);
	memcpy(payload->buffers[R615_BOOT_SIGNATURE].cpu, fw.signature, fw.signature_bytes);
	config.chipset = 0x177;
	config.fb_bytes = environment->fb_bytes;
	config.vga_start = environment->vga_start;
	config.vga_valid = environment->vga_valid;
	config.mmu_lock_start = environment->mmu_lock_start;
	config.mmu_lock_valid = environment->mmu_lock_valid;
	config.boost = environment->boost;
	config.elf_radix = elf_address;
	config.elf_bytes = elf_bytes;
	config.boot_address = payload->buffers[R615_BOOT_LOADER].address;
	config.boot_bytes = boot.image_bytes;
	config.boot_code = boot.code_offset;
	config.boot_data = boot.data_offset;
	config.boot_manifest = boot.manifest_offset;
	config.signature_address = payload->buffers[R615_BOOT_SIGNATURE].address;
	config.signature_bytes = fw.signature_bytes;
	ret = r615_wpr_build(gsp->rm->version, &config, payload->buffers[R615_BOOT_WPR].cpu);
	if (ret)
		goto fail;
	for (u32 i = 0; i < R615_BOOT_LOG_COUNT; i++) {
		struct r615_boot_buffer *buffer = &payload->buffers[R615_BOOT_LOG_INIT + i];

		/* LibOS logging ABI: word zero is the put pointer, word one is its DMA PA. */
		memcpy((u8 *)buffer->cpu + sizeof(u64), &buffer->address, sizeof(u64));
		regions[i] = (struct r615_libos_region) { .id = r615_libos_id(r615_boot_logs[i].id),
			.address = buffer->address, .bytes = buffer->bytes,
			.kind = 1, .location = 1 };
	}
	regions[R615_BOOT_LOG_COUNT] = (struct r615_libos_region) { .id = r615_libos_id("RMARGS"),
		.address = payload->buffers[R615_BOOT_ARGS].address,
		.bytes = R615_BOOT_PAGE, .kind = 1, .location = 1 };
	ret = r615_libos_build(regions, ARRAY_SIZE(regions),
			       payload->buffers[R615_BOOT_LIBOS].cpu, R615_BOOT_PAGE);
	if (ret)
		goto fail;
	dma_wmb();
	*output = payload;
	return 0;
fail:
	r615_boot_payload_del(&payload);
	return ret;
}

int
r615_boot_payload_request(struct nvkm_gsp *gsp, const struct r615_boot_environment *environment,
			  r615_transport_quiesce_func quiesce, void *context,
			  struct r615_boot_payload **output)
{
	const char * const names[] = {
		"nvidia/ga107/gsp/gsp-615.78.08.bin",
		"nvidia/ga107/gsp/bootloader-615.78.08.bin",
		"nvidia/ga107/gsp/ucodes-615.78.08.bin",
	};
	const struct firmware *files[3] = { 0 };
	int ret = -EINVAL;

	if (!gsp || !gsp->subdev.device || !gsp->subdev.device->dev || !output || *output)
		return -EINVAL;
	if (!gsp->rm || !gsp->rm->version || strcmp(gsp->rm->version, "615.78.08") ||
	    gsp->subdev.device->chipset != 0x177)
		return -EOPNOTSUPP;
	if (gsp->running)
		return -EBUSY;
	for (u32 i = 0; i < 3; i++) {
		ret = request_firmware(&files[i], names[i], gsp->subdev.device->dev);
		if (ret)
			goto done;
	}
	ret = r615_boot_payload_new(gsp, files[0], files[1], files[2], environment,
				   quiesce, context, output);
done:
	for (u32 i = 0; i < 3; i++)
		release_firmware(files[i]);
	return ret;
}

static void
r615_boot_payload_handoff(struct r615_boot_payload *payload, struct r615_boot_handoff *output)
{
	*output = (struct r615_boot_handoff) {
		.wpr_address = payload->buffers[R615_BOOT_WPR].address,
		.libos_address = payload->buffers[R615_BOOT_LIBOS].address,
		.args_address = payload->buffers[R615_BOOT_ARGS].address,
		.state_monitor_address = payload->buffers[R615_BOOT_STATE_MONITOR].address,
		.state_monitor_bytes = payload->buffers[R615_BOOT_STATE_MONITOR].bytes,
		.app_version = payload->app_version,
	};
}

int
r615_boot_payload_arm(struct r615_boot_payload *payload, struct r615_boot_handoff *output)
{
	struct r615_init_args args;
	int ret;

	if (!payload || !output)
		return -EINVAL;
	if (payload->host_disabled)
		return -ESHUTDOWN;
	if (payload->armed)
		return -EALREADY;
	/* No fallible operation or new allocation after exposing the transport. */
	ret = r615_transport_arm(payload->transport, &args);
	if (ret)
		return ret;
	args.bindata.address = payload->bindata_address;
	args.bindata.size = payload->bindata_bytes;
	/* Public 615 kgspAllocRmStateMonitorBuffer / kgspSetupGspInitArgs:
	 * this is a separate coherent buffer, not the LOGRMON byte queue.
	 * It is zeroed during allocation and lives until payload quiescence.
	 */
	args.state_monitor.address = payload->buffers[R615_BOOT_STATE_MONITOR].address;
	args.state_monitor.size = payload->buffers[R615_BOOT_STATE_MONITOR].bytes;
	memcpy(payload->buffers[R615_BOOT_ARGS].cpu, &args, sizeof(args));
	dma_wmb();
	payload->armed = true;
	r615_boot_payload_handoff(payload, output);
	return 0;
}

int
r615_boot_payload_pm_pause(struct r615_boot_payload *payload)
{
	if (!payload || !payload->armed)
		return -EINVAL;
	if (payload->host_disabled && !payload->pm_paused)
		return -ESHUTDOWN;
	payload->host_disabled = true;
	payload->pm_paused = true;
	return r615_transport_pm_pause(payload->transport);
}

int
r615_boot_payload_pm_resume(struct r615_boot_payload *payload, u32 old_level,
			    struct r615_boot_handoff *output)
{
	struct r615_init_args args;
	int ret;

	if (!payload || !output || !payload->armed || (old_level != 3 && old_level != 7))
		return -EINVAL;
	if (!payload->pm_paused || !payload->host_disabled)
		return -ESHUTDOWN;
	/* The transport verifies owner quiescence before permitting any RMARGS
	 * write. Do not read/modify the old firmware-shared RMARGS as an oracle.
	 */
	ret = r615_transport_pm_args(payload->transport, &args);
	if (ret)
		return ret;
	args.flags = R615_INIT_PM_TRANSITION;
	args.resume.old_level = old_level;
	args.resume.flags = 0;
	args.bindata.address = payload->bindata_address;
	args.bindata.size = payload->bindata_bytes;
	args.state_monitor.address = payload->buffers[R615_BOOT_STATE_MONITOR].address;
	args.state_monitor.size = payload->buffers[R615_BOOT_STATE_MONITOR].bytes;
	memcpy(payload->buffers[R615_BOOT_ARGS].cpu, &args, sizeof(args));
	dma_wmb();
	ret = r615_transport_pm_resume(payload->transport);
	if (ret)
		return ret;
	payload->host_disabled = false;
	payload->pm_paused = false;
	r615_boot_payload_handoff(payload, output);
	return 0;
}

struct r615_transport *
r615_boot_payload_transport(struct r615_boot_payload *payload)
{
	return payload && payload->armed ? payload->transport : NULL;
}

int
r615_boot_payload_wpr(struct r615_boot_payload *payload, struct r615_wpr_meta *output)
{
	if (!payload || !output)
		return -EINVAL;
	memcpy(output, payload->buffers[R615_BOOT_WPR].cpu, sizeof(*output));
	return 0;
}

const void *
r615_boot_payload_log_init(const struct r615_boot_payload *payload, size_t *bytes)
{
	if (!payload)
		return NULL;
	if (bytes)
		*bytes = payload->buffers[R615_BOOT_LOG_INIT].bytes;
	return payload->buffers[R615_BOOT_LOG_INIT].cpu;
}

int
r615_boot_payload_resume_info(const struct r615_boot_payload *payload, u64 *libos, u32 *version)
{
	if (!payload || !payload->armed || !libos || !version)
		return -EINVAL;
	if (payload->host_disabled)
		return -ESHUTDOWN;
	*libos = payload->buffers[R615_BOOT_LIBOS].address;
	*version = payload->app_version;
	return 0;
}

bool
r615_boot_payload_dma_owned(const struct r615_boot_payload *payload, u64 address, u64 bytes)
{
	if (!payload || !payload->armed || payload->host_disabled || !bytes || address > ~0ULL - bytes)
		return false;
	for (u32 i = 0; i < R615_BOOT_BUFFER_COUNT; i++) {
		const struct r615_boot_buffer *buffer = &payload->buffers[i];

		if (address >= buffer->address && address - buffer->address <= buffer->bytes &&
		    bytes <= buffer->bytes - (address - buffer->address))
			return true;
	}
	return r615_dma_image_owns(payload->image, address, bytes) ||
	       r615_dma_image_owns(payload->bindata, address, bytes);
}

const void *
r615_boot_payload_log(const struct r615_boot_payload *payload, u32 index,
		      const char **name, size_t *bytes)
{
	if (!payload || index >= R615_BOOT_LOG_COUNT || !name || !bytes)
		return NULL;
	*name = r615_boot_logs[index].name;
	*bytes = payload->buffers[R615_BOOT_LOG_INIT + index].bytes;
	return payload->buffers[R615_BOOT_LOG_INIT + index].cpu;
}

int
r615_boot_payload_quiesce(struct r615_boot_payload *payload)
{
	int ret;

	if (!payload)
		return -EINVAL;
	ret = r615_transport_quiesce(payload->transport);
	if (!ret)
		payload->host_disabled = true;
	return ret;
}

void
r615_boot_payload_quarantine(struct r615_boot_payload *payload)
{
	if (payload) {
		payload->host_disabled = true;
		r615_transport_quarantine(payload->transport);
	}
}

int
r615_boot_payload_del(struct r615_boot_payload **output)
{
	struct r615_boot_payload *payload;
	int ret;

	if (!output || !*output)
		return 0;
	payload = *output;
	/* Transport enforces successful owner quiescence for the entire boot payload. */
	ret = r615_transport_del(&payload->transport);
	if (ret)
		return ret;
	r615_dma_image_quiesced(payload->image);
	r615_dma_image_quiesced(payload->bindata);
	r615_dma_image_del(&payload->image);
	r615_dma_image_del(&payload->bindata);
	for (u32 i = 0; i < R615_BOOT_BUFFER_COUNT; i++) {
		struct r615_boot_buffer *buffer = &payload->buffers[i];

		if (buffer->cpu)
			dma_free_coherent(payload->device, buffer->bytes,
					  buffer->cpu, buffer->address);
	}
	put_device(payload->device);
	kfree(payload);
	*output = NULL;
	return 0;
}

MODULE_FIRMWARE("nvidia/ga107/gsp/gsp-615.78.08.bin");
MODULE_FIRMWARE("nvidia/ga107/gsp/bootloader-615.78.08.bin");
MODULE_FIRMWARE("nvidia/ga107/gsp/ucodes-615.78.08.bin");
