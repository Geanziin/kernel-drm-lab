// SPDX-License-Identifier: MIT
/* Retain CPU copies after a failed bringup, never expose freed DMA to debugfs. */
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <core/device.h>
#include <nvif/log.h>
#include <rm/rm.h>
#include "boot_payload.h"
#include "lifecycle.h"
#include "runtime.h"
#include "libos.h"

#ifdef CONFIG_DEBUG_FS
extern struct dentry *nouveau_debugfs_root;

struct r615_saved_logs {
	struct nvif_log lifetime;
	struct dentry *directory;
	struct debugfs_blob_wrapper buffers[R615_BOOT_LOG_COUNT + 1];
	bool linked;
};

static void
r615_saved_logs_shutdown(struct nvif_log *entry)
{
	struct r615_saved_logs *saved = container_of(entry, struct r615_saved_logs, lifetime);

	debugfs_remove(saved->directory);
	if (saved->linked)
		list_del(&saved->lifetime.entry);
	for (u32 i = 0; i < R615_BOOT_LOG_COUNT + 1; i++)
		kfree(saved->buffers[i].data);
	kfree(saved);
}

void
r615_gsp_save_logs(struct nvkm_gsp *gsp)
{
	struct r615_saved_logs *saved;
	char directory[64];
	u32 entries = 0;

	if (!gsp->r615_payload || !nouveau_debugfs_root)
		return;
	saved = kzalloc_obj(*saved, GFP_KERNEL);
	if (!saved)
		return;
	snprintf(directory, sizeof(directory), "r615-%s", dev_name(gsp->subdev.device->dev));
	saved->directory = debugfs_create_dir(directory, nouveau_debugfs_root);
	if (IS_ERR_OR_NULL(saved->directory)) {
		kfree(saved);
		return;
	}
	/* Shared DMA is readable here only after the owner proved GSP stop.
	 * If stop failed, retain only the independently copied PMU RPC snapshot.
	 * These are binary diagnostics, not decoded strings/progress counters.
	 */
	if (gsp->r615_stopped)
		dma_rmb();
	for (u32 i = 0; gsp->r615_stopped && i < R615_BOOT_LOG_COUNT; i++) {
		const char *name;
		size_t bytes;
		const void *data = r615_boot_payload_log(gsp->r615_payload, i, &name, &bytes);
		struct dentry *entry;

		if (!data || !bytes || bytes > 0x10000)
			goto fail;
		saved->buffers[i].data = kmemdup(data, bytes, GFP_KERNEL);
		if (!saved->buffers[i].data)
			goto fail;
		saved->buffers[i].size = bytes;
		entry = debugfs_create_blob(name, 0400, saved->directory, &saved->buffers[i]);
		if (IS_ERR_OR_NULL(entry))
			goto fail;
		i_size_write(d_inode(entry), bytes);
		entries++;
	}
	/* Last complete PMU RPC snapshot, independent of firmware DMA and of the
	 * runtime owner being destroyed after this call. Optional when no print
	 * event was received. Allocate outside the spinlock used by the copier.
	 */
	{
		struct debugfs_blob_wrapper *blob = &saved->buffers[R615_BOOT_LOG_COUNT];
		struct dentry *entry;
		u32 bytes;
		int ret;

		blob->data = kmalloc(R615_LIBOS_MAX_BYTES, GFP_KERNEL);
		if (!blob->data)
			goto fail;
		ret = r615_runtime_log_copy(gsp, blob->data, R615_LIBOS_MAX_BYTES, &bytes);
		if (ret == -ENOENT) {
			kfree(blob->data);
			blob->data = NULL;
		} else if (ret) {
			goto fail;
		} else {
			blob->size = bytes;
			entry = debugfs_create_blob("pmu-libos-raw", 0400, saved->directory, blob);
			if (IS_ERR_OR_NULL(entry))
				goto fail;
			i_size_write(d_inode(entry), bytes);
			entries++;
		}
	}
	if (!entries) {
		r615_saved_logs_shutdown(&saved->lifetime);
		return;
	}
	saved->lifetime.shutdown = r615_saved_logs_shutdown;
	/* Same module-exit lifetime list used by retained upstream GSP logs. */
	list_add(&saved->lifetime.entry, &gsp_logs.head);
	saved->linked = true;
	nvkm_info(&gsp->subdev, "r615: binary logs retained in debugfs nouveau/%s\n", directory);
	return;
fail:
	r615_saved_logs_shutdown(&saved->lifetime);
	nvkm_warn(&gsp->subdev, "r615: could not retain failure logs\n");
}
#else
void r615_gsp_save_logs(struct nvkm_gsp *gsp) {}
#endif
