// SPDX-License-Identifier: MIT
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "runtime.h"
#include "libos.h"
#include "health.h"

static_assert(sizeof(struct r615_intr_entry) == 16);
static_assert(sizeof(struct r615_intr_table) == 2112);
static_assert(sizeof(struct r615_post_event) == 32);
static_assert(sizeof(struct r615_rc_event) == 64);

int
r615_intr_table_validate(const struct r615_intr_table *table, u32 max_vector)
{
	if (!table || !max_vector)
		return -EINVAL;
	if (!table->count || table->count > R615_INTR_ENTRIES)
		return -EPROTO;
	for (u32 i = 0; i < table->count; i++) {
		const struct r615_intr_entry *entry = &table->entries[i];

		if ((entry->stall != ~0U && entry->stall >= max_vector) ||
		    (entry->nonstall != ~0U && entry->nonstall >= max_vector))
			return -EPROTO;
		for (u32 j = 0; j < i; j++) {
			if (table->entries[j].engine == entry->engine)
				return -EPROTO;
		}
	}
	return 0;
}

int
r615_post_event_validate(const void *data, u32 bytes)
{
	struct r615_post_event header;

	if (!data || bytes < sizeof(header))
		return -EPROTO;
	memcpy(&header, data, sizeof(header));
	if (header.bytes != bytes - sizeof(header) || header.notify_list > 1)
		return -EPROTO;
	return 0;
}

bool
r615_event_native(u32 function)
{
	switch (function) {
	case R615_GMCAPI_MMU_FAULT_QUEUED:
	case R615_EVENT_OLD_INIT:
	case R615_EVENT_OLD_SEQUENCER:
	case R615_EVENT_OLD_GENERIC:
	case R615_EVENT_OLD_HS:
	case R615_EVENT_POST:
	case R615_EVENT_RC:
	case R615_EVENT_ERROR:
	case R615_EVENT_MMU:
	case R615_EVENT_LIBOS_PRINT:
	case R615_EVENT_LOCKDOWN:
	case R615_EVENT_NOCAT:
	case R615_EVENT_PMU_HALTED:
	case R615_EVENT_FORCED_SHUTDOWN:
	case R615_EVENT_RECOVERY:
	case R615_EVENT_FECS:
	case R615_EVENT_BUGCHECK:
		return true;
	default:
		return false;
	}
}

int
r615_event_validate(u32 function, const void *data, u32 bytes)
{
	struct r615_rc_event rc;
	struct r615_recovery_event recovery;
	struct r615_fecs_event fecs;
	u32 diagnostic_bytes;

	if (!data && bytes)
		return -EPROTO;
	switch (function) {
	case R615_EVENT_POST:
		return r615_post_event_validate(data, bytes);
	case R615_EVENT_RC:
		if (bytes < sizeof(rc))
			return -EPROTO;
		memcpy(&rc, data, sizeof(rc));
		if (rc.journal_bytes != bytes - sizeof(rc) || rc.callback_needed > 1 ||
		    rc.info_type > 1 || (!rc.info_type && rc.chid > 0x7fffffffU) ||
		    (rc.info_type && !rc.inst_lo && !rc.inst_hi))
			return -EPROTO;
		return rc.gfid ? -EOPNOTSUPP : 0;
	case R615_EVENT_ERROR:
		return bytes == sizeof(struct r615_error_event) ? 0 : -EPROTO;
	case R615_EVENT_MMU:
		return bytes ? -EPROTO : 0;
	case R615_EVENT_LOCKDOWN:
		return bytes == 1 && *(const u8 *)data <= 1 ? 0 : -EPROTO;
	case R615_EVENT_NOCAT:
		if (bytes != sizeof(struct r615_nocat_event))
			return -EPROTO;
		memcpy(&diagnostic_bytes,
		       (const u8 *)data + offsetof(struct r615_nocat_event, diagnostic_bytes),
		       sizeof(diagnostic_bytes));
		return diagnostic_bytes <= sizeof(((struct r615_nocat_event *)0)->diagnostic) ? 0 : -EPROTO;
	case R615_EVENT_PMU_HALTED:
	case R615_EVENT_FORCED_SHUTDOWN:
		return bytes ? -EPROTO : 0;
	case R615_EVENT_LIBOS_PRINT:
		return r615_libos_validate(data, bytes);
	case R615_EVENT_BUGCHECK:
		return bytes == sizeof(u32) ? 0 : -EPROTO;
	case R615_EVENT_RECOVERY:
		if (bytes != sizeof(recovery))
			return -EPROTO;
		memcpy(&recovery, data, sizeof(recovery));
		return recovery.type <= 4 && recovery.value <= 1 ? 0 : -EPROTO;
	case R615_EVENT_FECS:
		if (bytes != sizeof(fecs))
			return -EPROTO;
		memcpy(&fecs, data, sizeof(fecs));
		if (fecs.type < 1 || fecs.type > 2)
			return -EPROTO;
		return fecs.gr ? -EOPNOTSUPP : 0;
	default:
		return -EOPNOTSUPP;
	}
}
