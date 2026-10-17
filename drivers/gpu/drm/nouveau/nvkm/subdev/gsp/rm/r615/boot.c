/* SPDX-License-Identifier: MIT */
#include <linux/errno.h>
#include <linux/string.h>
#include "boot.h"

int
r615_build_init_args(const struct r615_boot_config *config, struct r615_init_args *output)
{
	struct r615_init_args args = { 0 };
	const struct r615_queue_args *queue;
	u64 alignment, pte_bytes;

	if (!config || !output)
		return -EINVAL;
	queue = &config->queue;
	/* Encrypted/CC queues need their own authenticated transport implementation. */
	if (queue->flags)
		return -EOPNOTSUPP;
	if (!queue->pte_count || !config->shared_mem_bytes ||
	    !queue->element_header_bytes || !queue->element_min_bytes ||
	    queue->element_header_bytes > queue->element_min_bytes ||
	    queue->element_min_bytes > queue->element_max_bytes ||
	    queue->header_alignment_log2 >= 32 || queue->element_alignment_log2 >= 32)
		return -EINVAL;
	if (!queue->shared_mem_pa || queue->shared_mem_pa % 4096 ||
	    config->shared_mem_bytes % 4096 ||
	    queue->pte_count != config->shared_mem_bytes / 4096)
		return -EINVAL;
	if (queue->element_header_bytes != 24)
		return -EOPNOTSUPP;
	alignment = 1ULL << queue->element_alignment_log2;
	pte_bytes = (u64)queue->pte_count * sizeof(u64);
	if (queue->cmd_offset < pte_bytes || queue->status_offset <= queue->cmd_offset ||
	    queue->status_offset >= config->shared_mem_bytes ||
	    queue->cmd_offset % alignment || queue->status_offset % alignment ||
	    queue->element_min_bytes % alignment || queue->element_max_bytes % alignment ||
	    queue->status_offset - queue->cmd_offset < queue->element_max_bytes ||
	    config->shared_mem_bytes - queue->status_offset < queue->element_max_bytes)
		return -EINVAL;
	if (!config->resume && (config->old_level || config->pm_flags))
		return -EINVAL;

	args.magic = R615_INIT_MAGIC;
	args.size = sizeof(args);
	args.queue = *queue;
	memset(args.queue.reserved, 0, sizeof(args.queue.reserved));
	args.gpu_instance = config->gpu_instance;
	if (config->resume) {
		args.flags = R615_INIT_PM_TRANSITION;
		args.resume.old_level = config->old_level;
		args.resume.flags = config->pm_flags;
	} else if (config->dmem_stack) {
		args.flags = R615_INIT_DMEM_STACK;
	}
	*output = args;
	return 0;
}
