// SPDX-License-Identifier: MIT
#include <linux/array_size.h>
#include <linux/elf.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "firmware.h"

static bool
r615_firmware_span(u64 offset, u64 length, size_t bytes)
{
	return offset <= bytes && length <= bytes - offset;
}

static bool
r615_firmware_output(const void *data, size_t bytes, const void *output, size_t output_bytes)
{
	unsigned long a = (unsigned long)data, b = (unsigned long)output;

	return a <= ~0UL - bytes && b <= ~0UL - output_bytes &&
	       (a + bytes <= b || b + output_bytes <= a);
}

int
r615_firmware_parse(const void *data, size_t bytes, struct r615_firmware_view *output)
{
	struct r615_firmware_view view = { 0 };
	struct elf64_hdr header;
	struct elf64_shdr strings;
	const char *names;
	const void *version = NULL;
	bool version_seen = false;

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	return -EOPNOTSUPP;
#endif
	if (!data || !output || bytes < sizeof(header) ||
	    !r615_firmware_output(data, bytes, output, sizeof(*output)))
		return -EINVAL;
	memcpy(&header, data, sizeof(header));
	if (memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
	    header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
	    header.e_type != ET_REL || header.e_machine != EM_RISCV ||
	    header.e_version != EV_CURRENT || header.e_ehsize != sizeof(header) ||
	    header.e_shentsize != sizeof(struct elf64_shdr) || !header.e_shnum ||
	    header.e_shnum > 512 || !header.e_shstrndx || header.e_shstrndx >= header.e_shnum ||
	    header.e_shoff < sizeof(header) ||
	    !r615_firmware_span(header.e_shoff, (u64)header.e_shnum * header.e_shentsize, bytes))
		return -ENOEXEC;
	memcpy(&strings, (const u8 *)data + header.e_shoff +
	       (u64)header.e_shstrndx * header.e_shentsize, sizeof(strings));
	if (strings.sh_type != SHT_STRTAB || !strings.sh_size ||
	    strings.sh_offset < sizeof(header) ||
	    !r615_firmware_span(strings.sh_offset, strings.sh_size, bytes))
		return -ENOEXEC;
	names = (const char *)data + strings.sh_offset;
	if (names[0] || names[strings.sh_size - 1])
		return -ENOEXEC;
	for (u32 i = 0; i < header.e_shnum; i++) {
		struct elf64_shdr section;
		const char *name;
		const void *payload;

		memcpy(&section, (const u8 *)data + header.e_shoff +
		       (u64)i * header.e_shentsize, sizeof(section));
		if (section.sh_name >= strings.sh_size ||
		    !memchr(names + section.sh_name, 0, strings.sh_size - section.sh_name) ||
		    (section.sh_type != SHT_NOBITS &&
		     !r615_firmware_span(section.sh_offset, section.sh_size, bytes)))
			return -ENOEXEC;
		name = names + section.sh_name;
		if (strcmp(name, ".fwimage") && strcmp(name, ".fwversion") &&
		    strcmp(name, ".fwsignature_ga10x"))
			continue;
		if (section.sh_type != SHT_PROGBITS || section.sh_offset < sizeof(header) ||
		    !section.sh_size ||
		    (section.sh_flags & ~(SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR)) ||
		    !r615_firmware_output((const u8 *)data + section.sh_offset, section.sh_size,
				 (const u8 *)data + header.e_shoff,
				 (size_t)header.e_shnum * header.e_shentsize) ||
		    !r615_firmware_output((const u8 *)data + section.sh_offset, section.sh_size,
				 names, strings.sh_size))
			return -ENOEXEC;
		payload = (const u8 *)data + section.sh_offset;
		if (!strcmp(name, ".fwimage")) {
			if (view.image)
				return -ENOEXEC;
			view.image = payload;
			view.image_bytes = section.sh_size;
		} else if (!strcmp(name, ".fwsignature_ga10x")) {
			if (view.signature || section.sh_size != 4096)
				return -ENOEXEC;
			view.signature = payload;
			view.signature_bytes = section.sh_size;
		} else {
			if (version_seen || section.sh_size != sizeof("615.78.08") ||
			    memcmp(payload, "615.78.08", sizeof("615.78.08")))
				return -EPROTO;
			version_seen = true;
			version = payload;
		}
	}
	if (!view.image || !view.signature || !version_seen)
		return -ENOEXEC;
	if (!r615_firmware_output(view.image, view.image_bytes,
				 view.signature, view.signature_bytes) ||
	    !r615_firmware_output(view.image, view.image_bytes, version, sizeof("615.78.08")) ||
	    !r615_firmware_output(view.signature, view.signature_bytes,
				 version, sizeof("615.78.08")))
		return -ENOEXEC;
	*output = view;
	return 0;
}

int
r615_bootloader_parse(const void *data, size_t bytes, struct r615_bootloader_view *output)
{
	const u32 pairs[] = { 1, 3, 5, 8, 10, 12, 15, 17 };
	u32 header[6], descriptor[23] = { 0 };
	struct r615_bootloader_view view = { 0 };
	u32 desc_bytes;

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	return -EOPNOTSUPP;
#endif
	if (!data || !output || bytes < sizeof(header) ||
	    !r615_firmware_output(data, bytes, output, sizeof(*output)))
		return -EINVAL;
	memcpy(header, data, sizeof(header));
	/* The public generator declares a 256-byte aligned total but omits tail padding. */
	if (bytes > 0xffffff00U || header[0] != 0x10de || header[1] != 1 ||
	    header[2] != ((bytes + 255) & ~255ULL) || header[3] != sizeof(header) ||
	    header[4] < sizeof(header) || header[4] > bytes ||
	    !header[5] || header[5] != bytes - header[4])
		return -ENOEXEC;
	desc_bytes = header[4] - header[3];
	/* The GA102 production v5 descriptor is the 84-byte prefix of the 92-byte
	 * public type. Missing SMP/PLIC fields are zero, never read beyond its end.
	 */
	if (desc_bytes != 84 && desc_bytes != sizeof(descriptor))
		return -EOPNOTSUPP;
	memcpy(descriptor, (const u8 *)data + header[3], desc_bytes);
	if (descriptor[0] != 5 || descriptor[14] != 1 || descriptor[20] ||
	    descriptor[21] || descriptor[22])
		return -EOPNOTSUPP;
	/* Bootloader, parameters, ELF, manifest, monitor and SWBROM offset/size pairs. */
	for (u32 i = 0; i < ARRAY_SIZE(pairs); i++) {
		u32 index = pairs[i];

		if (!r615_firmware_span(descriptor[index], descriptor[index + 1], header[5]))
			return -ENOEXEC;
	}
	if (!descriptor[2] || !descriptor[9] || !descriptor[11] || !descriptor[13] ||
	    descriptor[19] < header[5])
		return -ENOEXEC;
	view.image = (const u8 *)data + header[4];
	view.image_bytes = header[5];
	view.code_offset = descriptor[12];
	view.data_offset = descriptor[10];
	view.manifest_offset = descriptor[8];
	view.app_version = descriptor[7];
	*output = view;
	return 0;
}
