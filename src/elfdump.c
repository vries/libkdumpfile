/** @internal @file src/elfdump.c
 * @brief Routines to work with ELF kdump files.
 */
/* Copyright (C) 2014 Petr Tesarik <ptesarik@suse.cz>

   This file is free software; you can redistribute it and/or modify
   it under the terms of either

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at
       your option) any later version

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at
       your option) any later version

   or both in parallel, as here.

   libkdumpfile is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE

#include "kdumpfile-priv.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <elf.h>

/* This definition is missing from older version of <elf.h> */
#ifndef EM_AARCH64
# define EM_AARCH64      183
#endif

static const struct format_ops xc_core_elf_ops;

struct xen_p2m {
	uint64_t pfn;
	uint64_t gmfn;
};

struct load_segment {
	off_t file_offset;
	off_t filesz;
	kdump_paddr_t phys;
	kdump_addr_t memsz;
	kdump_vaddr_t virt;
};

struct section {
	off_t file_offset;
	uint64_t size;
	int name_index;
};

struct elfdump_priv {
	int num_load_segments;
	struct load_segment *load_segments;
	int num_note_segments;
	struct load_segment *note_segments;

	int num_sections;
	struct section *sections;

	size_t strtab_size;
	char *strtab;

	off_t xen_pages_offset;

	int elfclass;
};

static void elf_cleanup(struct kdump_shared *shared);

static const char *
mach2arch(unsigned mach, int elfclass)
{
	switch(mach) {
	case EM_AARCH64:
			return KDUMP_ARCH_AARCH64;
	case EM_ARM:	return KDUMP_ARCH_ARM;
	case EM_ALPHA:
	case EM_FAKE_ALPHA:
			return KDUMP_ARCH_ALPHA;
	case EM_IA_64:	return KDUMP_ARCH_IA64;
	case EM_MIPS:	return KDUMP_ARCH_MIPS;
	case EM_PPC:	return KDUMP_ARCH_PPC;
	case EM_PPC64:	return KDUMP_ARCH_PPC64;
	case EM_S390:	return (elfclass == ELFCLASS64
				? KDUMP_ARCH_S390X
				: KDUMP_ARCH_S390);
	case EM_386:	return KDUMP_ARCH_IA32;
	case EM_X86_64:	return KDUMP_ARCH_X86_64;
	default:	return NULL;
	}
}

/**  Find the LOAD segment that is closest to a physical address.
 * @param edp	 ELF dump private data.
 * @param paddr	 Requested physical address.
 * @param dist	 Maximum allowed distance from @ref paddr.
 * @returns	 Pointer to the closest LOAD segment, or @c NULL if none.
 */
static struct load_segment *
find_closest_load(struct elfdump_priv *edp, kdump_paddr_t paddr,
		  unsigned long dist)
{
	unsigned long bestdist;
	struct load_segment *bestload;
	int i;

	bestdist = dist;
	bestload = NULL;
	for (i = 0; i < edp->num_load_segments; i++) {
		struct load_segment *pls = &edp->load_segments[i];
		if (paddr >= pls->phys + pls->memsz)
			continue;
		if (paddr >= pls->phys)
			return pls;	/* Exact match */
		if (bestdist > pls->phys - paddr) {
			bestdist = pls->phys - paddr;
			bestload = pls;
		}
	}
	return bestload;
}

static kdump_status
elf_read_page(kdump_ctx_t *ctx, struct page_io *pio, cache_key_t addr)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	struct load_segment *pls;
	void *p, *endp;
	off_t pos;
	ssize_t size, rd;

	p = pio->chunk.data;
	endp = p + get_page_size(ctx);
	while (p < endp) {
		pls = find_closest_load(edp, addr, endp - p);
		if (!pls) {
			memset(p, 0, endp - p);
			break;
		}

		if (pls->phys > addr) {
			memset(p, 0, pls->phys - addr);
			p += pls->phys - addr;
			addr = pls->phys;
		}

		pos = pls->file_offset + addr - pls->phys;
		if (pls->phys + pls->filesz > addr) {
			size = endp - p;
			if (size > pls->phys + pls->filesz - addr)
				size = pls->phys + pls->filesz - addr;

			rd = pread(get_file_fd(ctx), p, size, pos);
			if (rd != size)
				return set_error(
					ctx, read_error(rd),
					"Cannot read page data at %llu",
					(unsigned long long) pos);
			p += size;
			addr += size;
		}
		if (p < endp) {
			size = endp - p;
			if (size > pls->phys + pls->memsz - addr)
				size = pls->phys + pls->memsz - addr;
			memset(p, 0, size);
			p += size;
			addr += size;
		}
	}

	return KDUMP_OK;
}

static kdump_status
elf_get_page(kdump_ctx_t *ctx, struct page_io *pio)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	struct load_segment *pls;
	kdump_paddr_t addr;
	size_t sz;

	pls = find_closest_load(edp, pio->addr.addr, get_page_size(ctx));
	if (!pls)
		return set_error(ctx, KDUMP_ERR_NODATA, "Page not found");

	addr = pio->addr.addr;
	sz = get_page_size(ctx);
	return (pls->phys <= addr && pls->filesz >= addr - pls->phys + sz)
		? fcache_get_chunk(ctx->shared->fcache, &pio->chunk, sz,
				   pls->file_offset + addr - pls->phys)
		: cache_get_page(ctx, pio, elf_read_page, addr);
}

static void
get_max_pfn_xen_auto(kdump_ctx_t *ctx)
{
	uint64_t *p;
	unsigned long i;
	kdump_pfn_t max_pfn = 0;

	for (i = 0, p = ctx->shared->xen_map; i < ctx->shared->xen_map_size; ++i, ++p)
		if (*p >= max_pfn)
			max_pfn = *p + 1;

	set_max_pfn(ctx, max_pfn);
}

static void
get_max_pfn_xen_nonauto(kdump_ctx_t *ctx)
{
	struct xen_p2m *p;
	unsigned long i;
	kdump_pfn_t max_pfn = 0;

	for (i = 0, p = ctx->shared->xen_map; i < ctx->shared->xen_map_size;
	     ++i, ++p)
		if (p->pfn >= max_pfn)
			max_pfn = p->pfn + 1;

	set_max_pfn(ctx, max_pfn);
}

static unsigned long
pfn_to_idx(kdump_ctx_t *ctx, kdump_pfn_t pfn)
{
	unsigned long i;

	if (get_xen_xlat(ctx) == KDUMP_XEN_AUTO) {
		uint64_t *p = ctx->shared->xen_map;
		for (i = 0; i < ctx->shared->xen_map_size; ++i, ++p)
			if (*p == pfn)
				return i;
	} else {
		struct xen_p2m *p = ctx->shared->xen_map;
		for (i = 0; i < ctx->shared->xen_map_size; ++i, ++p)
			if (p->pfn == pfn)
				return i;
	}

	return ~0UL;
}

static unsigned long
mfn_to_idx(kdump_ctx_t *ctx, kdump_pfn_t mfn)
{
	unsigned long i;

	if (get_xen_xlat(ctx) == KDUMP_XEN_NONAUTO) {
		struct xen_p2m *p = ctx->shared->xen_map;
		for (i = 0; i < ctx->shared->xen_map_size; ++i, ++p)
			if (p->gmfn == mfn)
				return i;
	}

	return ~0UL;
}

/** xc_core physical-to-machine first step function.
 * @param step  Step state.
 * @param addr  Address to be translated.
 * @returns     Error status.
 */
static addrxlat_status
xc_p2m_first_step(addrxlat_step_t *step, addrxlat_addr_t addr)
{
	const addrxlat_meth_t *meth = step->meth;
	struct kdump_shared *shared = meth->param.custom.data;
	struct xen_p2m *p = shared->xen_map;
	addrxlat_addr_t pfn = addr >> shared->page_shift.number;
	unsigned long i;

	for (i = 0; i < shared->xen_map_size; ++i, ++p)
		if (p->pfn == pfn) {
			step->base.addr = p->gmfn << shared->page_shift.number;
			step->idx[0] = addr & (shared->page_size.number - 1);
			step->remain = 1;
			step->elemsz = 1;
			return ADDRXLAT_OK;
		}

	return addrxlat_ctx_err(step->ctx, ADDRXLAT_ERR_NODATA,
				"PFN not found");
}

/** xc_core machine-to-physical first step function.
 * @param step  Step state.
 * @param addr  Address to be translated.
 * @returns     Error status.
 */
static addrxlat_status
xc_m2p_first_step(addrxlat_step_t *step, addrxlat_addr_t addr)
{
	const addrxlat_meth_t *meth = step->meth;
	struct kdump_shared *shared = meth->param.custom.data;
	struct xen_p2m *p = shared->xen_map;
	addrxlat_addr_t mfn = addr >> shared->page_shift.number;
	unsigned long i;

	for (i = 0; i < shared->xen_map_size; ++i, ++p)
		if (p->gmfn == mfn) {
			step->base.addr = p->pfn << shared->page_shift.number;
			step->idx[0] = addr & (shared->page_size.number - 1);
			step->remain = 1;
			step->elemsz = 1;
			return ADDRXLAT_OK;
		}

	return addrxlat_ctx_err(step->ctx, ADDRXLAT_ERR_NODATA,
				"MFN not found");
}

/** Identity next step function.
 * @param walk  Current step state.
 * @returns     Error status.
 *
 * This method does not modify anything and always succeeds.
 */
static addrxlat_status
next_step_ident(addrxlat_step_t *state)
{
	return ADDRXLAT_OK;
}

static kdump_status
setup_custom_method(kdump_ctx_t *ctx, addrxlat_sys_meth_t methidx,
		    addrxlat_sys_map_t mapidx, const addrxlat_meth_t *meth)
{
	addrxlat_range_t range;
	addrxlat_map_t *map;
	addrxlat_status axstatus;

	map = addrxlat_map_new();
	if (!map)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot allocate translation map");

	range.endoff = ADDRXLAT_ADDR_MAX;
	range.meth = methidx;
	axstatus = addrxlat_map_set(map, 0, &range);
	if (axstatus != ADDRXLAT_OK) {
		addrxlat_map_decref(map);
		return addrxlat2kdump(ctx, axstatus);
	}
	addrxlat_sys_set_map(ctx->shared->xlatsys, mapidx, map);
	addrxlat_sys_set_meth(ctx->shared->xlatsys, methidx, meth);

	return KDUMP_OK;
}

static kdump_status
xc_post_addrxlat(kdump_ctx_t *ctx)
{
	addrxlat_meth_t meth;
	kdump_status status;

	if (get_xen_xlat(ctx) != KDUMP_XEN_NONAUTO)
		return KDUMP_OK;

	/* common fields */
	meth.kind = ADDRXLAT_CUSTOM;
	meth.param.custom.next_step = next_step_ident;
	meth.param.custom.data = ctx->shared;

	/* p2m translation */
	meth.target_as = ADDRXLAT_MACHPHYSADDR;
	meth.param.custom.first_step = xc_p2m_first_step;
	status = setup_custom_method(ctx, ADDRXLAT_SYS_METH_KPHYS_MACHPHYS,
				     ADDRXLAT_SYS_MAP_KPHYS_MACHPHYS, &meth);
	if (status != KDUMP_OK)
		return set_error(ctx, status, "Failed p2m setup");

	/* m2p translation */
	meth.target_as = ADDRXLAT_KPHYSADDR;
	meth.param.custom.first_step = xc_m2p_first_step;
	status = setup_custom_method(ctx, ADDRXLAT_SYS_METH_MACHPHYS_KPHYS,
				     ADDRXLAT_SYS_MAP_MACHPHYS_KPHYS, &meth);
	if (status != KDUMP_OK)
		return set_error(ctx, status, "Failed m2p setup");

	return KDUMP_OK;
}

static kdump_status
xc_get_page(kdump_ctx_t *ctx, struct page_io *pio)
{
	struct elfdump_priv *edp;
	kdump_pfn_t pfn = pio->addr.addr >> get_page_shift(ctx);
	unsigned long idx;
	off_t offset;

	idx = (pio->addr.as == ADDRXLAT_KPHYSADDR
	       ? pfn_to_idx(ctx, pfn)
	       : mfn_to_idx(ctx, pfn));
	if (idx == ~0UL)
		return set_error(ctx, KDUMP_ERR_NODATA, "Page not found");

	edp = ctx->shared->fmtdata;
	offset = edp->xen_pages_offset + ((off_t)idx << get_page_shift(ctx));
	return fcache_get_chunk(ctx->shared->fcache, &pio->chunk,
				get_page_size(ctx), offset);
}

static kdump_status
init_segments(kdump_ctx_t *ctx, unsigned phnum)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;

	if (!phnum)
		return KDUMP_OK;

	edp->load_segments =
		ctx_malloc(2 * phnum * sizeof(struct load_segment),
			   ctx, "program headers");
	if (!edp->load_segments)
		return KDUMP_ERR_SYSTEM;
	edp->num_load_segments = 0;

	edp->note_segments = edp->load_segments + phnum;
	edp->num_note_segments = 0;
	return KDUMP_OK;
}

static kdump_status
init_sections(kdump_ctx_t *ctx, unsigned snum)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;

	if (!snum)
		return KDUMP_OK;

	edp->sections =
		ctx_malloc(snum * sizeof(struct section),
			   ctx, "section headers");
	if (!edp->sections)
		return KDUMP_ERR_SYSTEM;
	edp->num_sections = 0;
	return KDUMP_OK;
}

static struct load_segment *
next_phdr(struct elfdump_priv *edp, unsigned type)
{
	struct load_segment *pls;

	if (type == PT_LOAD) {
		pls = edp->load_segments + edp->num_load_segments;
		++edp->num_load_segments;
	} else if (type == PT_NOTE) {
		pls = edp->note_segments + edp->num_note_segments;
		++edp->num_note_segments;
	} else
		pls = NULL;

	return pls;
}

static void
store_sect(struct elfdump_priv *edp, off_t offset,
	   uint64_t size, unsigned name_index)
{
	struct section *ps;

	ps = edp->sections + edp->num_sections;
	ps->file_offset = offset;
	ps->size = size;
	ps->name_index = name_index;
	++edp->num_sections;
}

static void *
read_elf_sect(kdump_ctx_t *ctx, struct section *sect)
{
	void *buf;

	buf = ctx_malloc(sect->size, ctx, "ELF section buffer");
	if (!buf)
		return NULL;

	if (pread(get_file_fd(ctx), buf, sect->size,
		  sect->file_offset) == sect->size)
		return buf;

	free(buf);
	return NULL;
}

static kdump_status
init_strtab(kdump_ctx_t *ctx, unsigned strtabidx)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	struct section *ps;

	if (!strtabidx || strtabidx >= edp->num_sections)
		return KDUMP_OK;	/* no string table */

	ps = edp->sections + strtabidx;
	edp->strtab_size = ps->size;
	edp->strtab = read_elf_sect(ctx, ps);
	if (!edp->strtab)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot allocate string table (%zu bytes)",
				 edp->strtab_size);

	return KDUMP_OK;
}

static const char *
strtab_entry(struct elfdump_priv *edp, unsigned index)
{
	return index < edp->strtab_size
		? edp->strtab + index
		: NULL;
}

static kdump_status
init_elf32(kdump_ctx_t *ctx, Elf32_Ehdr *ehdr)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	Elf32_Phdr prog;
	Elf32_Shdr sect;
	off_t offset;
	kdump_status ret;
	int i;

	set_arch_machine(ctx, dump16toh(ctx, ehdr->e_machine));

	ret = init_segments(ctx, dump16toh(ctx, ehdr->e_phnum));
	if (ret != KDUMP_OK)
		return ret;

	ret = init_sections(ctx, dump16toh(ctx, ehdr->e_shnum));
	if (ret != KDUMP_OK)
		return ret;

	offset = dump32toh(ctx, ehdr->e_phoff);
	if (lseek(get_file_fd(ctx), offset, SEEK_SET) < 0)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot seek to program headers at %llu",
				 (unsigned long long) offset);
	for (i = 0; i < dump16toh(ctx, ehdr->e_phnum); ++i) {
		struct load_segment *pls;
		ssize_t rd;

		rd = read(get_file_fd(ctx), &prog, sizeof prog);
		if (rd != sizeof prog)
			return set_error(ctx, read_error(rd),
					 "Cannot read program header #%d", i);

		pls = next_phdr(edp, dump32toh(ctx, prog.p_type));
		if (pls) {
			pls->file_offset = dump32toh(ctx, prog.p_offset);
			pls->filesz = dump32toh(ctx, prog.p_filesz);
			pls->phys = dump32toh(ctx, prog.p_paddr);
			pls->memsz = dump32toh(ctx, prog.p_memsz);
			pls->virt = dump32toh(ctx, prog.p_vaddr);
		}
	}

	offset = dump32toh(ctx, ehdr->e_shoff);
	if (lseek(get_file_fd(ctx), offset, SEEK_SET) < 0)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot seek to section headers at %llu",
				 (unsigned long long) offset);
	for (i = 0; i < dump16toh(ctx, ehdr->e_shnum); ++i) {
		ssize_t rd;

		rd = read(get_file_fd(ctx), &sect, sizeof sect);
		if (rd != sizeof sect)
			return set_error(ctx, read_error(rd),
					 "Cannot read section header #%d", i);
		store_sect(edp,
			   dump32toh(ctx, sect.sh_offset),
			   dump32toh(ctx, sect.sh_size),
			   dump32toh(ctx, sect.sh_name));
	}

	ret = init_strtab(ctx, dump16toh(ctx, ehdr->e_shstrndx));
	if (ret != KDUMP_OK)
		return ret;

	return ret;
}

static kdump_status
init_elf64(kdump_ctx_t *ctx, Elf64_Ehdr *ehdr)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	Elf64_Phdr prog;
	Elf64_Shdr sect;
	off_t offset;
	kdump_status ret;
	int i;

	set_arch_machine(ctx, dump16toh(ctx, ehdr->e_machine));

	ret = init_segments(ctx, dump16toh(ctx, ehdr->e_phnum));
	if (ret != KDUMP_OK)
		return ret;

	ret = init_sections(ctx, dump16toh(ctx, ehdr->e_shnum));
	if (ret != KDUMP_OK)
		return ret;


	offset = dump64toh(ctx, ehdr->e_phoff);
	if (lseek(get_file_fd(ctx), offset, SEEK_SET) < 0)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot seek to program headers at %llu",
				 (unsigned long long) offset);
	for (i = 0; i < dump16toh(ctx, ehdr->e_phnum); ++i) {
		struct load_segment *pls;
		ssize_t rd;

		rd = read(get_file_fd(ctx), &prog, sizeof prog);
		if (rd != sizeof prog)
			return set_error(ctx, read_error(rd),
					 "Cannot read program header #%d", i);

		pls = next_phdr(edp, dump32toh(ctx, prog.p_type));
		if (pls) {
			pls->file_offset = dump64toh(ctx, prog.p_offset);
			pls->filesz = dump64toh(ctx, prog.p_filesz);
			pls->phys = dump64toh(ctx, prog.p_paddr);
			pls->memsz = dump64toh(ctx, prog.p_memsz);
			pls->virt = dump64toh(ctx, prog.p_vaddr);
		}
	}

	offset = dump32toh(ctx, ehdr->e_shoff);
	if (lseek(get_file_fd(ctx), offset, SEEK_SET) < 0)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot seek to section headers at %llu",
				 (unsigned long long) offset);
	for (i = 0; i < dump16toh(ctx, ehdr->e_shnum); ++i) {
		ssize_t rd;

		rd = read(get_file_fd(ctx), &sect, sizeof sect);
		if (rd != sizeof sect)
			return set_error(ctx, read_error(rd),
					 "Cannot read section header #%d", i);
		store_sect(edp,
			   dump64toh(ctx, sect.sh_offset),
			   dump64toh(ctx, sect.sh_size),
			   dump32toh(ctx, sect.sh_name));
	}

	ret = init_strtab(ctx, dump16toh(ctx, ehdr->e_shstrndx));
	if (ret != KDUMP_OK)
		return ret;

	return ret;
}

typedef kdump_status walk_notes_fn(kdump_ctx_t *, void *, size_t);

static kdump_status
walk_elf_notes(kdump_ctx_t *ctx, walk_notes_fn *fn)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	unsigned i;
	struct fcache_chunk fch;
	kdump_status ret;

	for (i = 0; i < edp->num_note_segments; ++i) {
		struct load_segment *seg = edp->note_segments + i;

		ret = fcache_get_chunk(ctx->shared->fcache, &fch,
				       seg->filesz, seg->file_offset);
		if (ret != KDUMP_OK)
			return set_error(ctx, ret,
					 "Cannot read ELF notes at %llu",
					 (unsigned long long) seg->file_offset);

		ret = fn(ctx, fch.data, seg->filesz);
		fcache_put_chunk(&fch);
		if (ret != KDUMP_OK)
			return ret;
	}

	return KDUMP_OK;
}

static kdump_status
open_common(kdump_ctx_t *ctx)
{
	struct elfdump_priv *edp = ctx->shared->fmtdata;
	kdump_status ret;
	int i;

	if (!edp->num_load_segments && !edp->num_sections)
		return set_error(ctx, KDUMP_ERR_NOTIMPL, "No content found");

	/* process NOTE segments */
	ret = walk_elf_notes(ctx, process_noarch_notes);
	if (ret != KDUMP_OK)
		return ret;

	if (!isset_arch_name(ctx)) {
		uint_fast16_t mach = get_arch_machine(ctx);
		const char *arch = mach2arch(mach, edp->elfclass);
		if (arch) {
			ret = set_arch_name(ctx, arch);
			if (ret != KDUMP_OK)
				return ret;
		}
	}

	ret = walk_elf_notes(ctx, process_arch_notes);
	if (ret != KDUMP_OK)
		return ret;

	/* process LOAD segments */
	for (i = 0; i < edp->num_load_segments; ++i) {
		struct load_segment *seg = edp->load_segments + i;
		unsigned long pfn =
			(seg->phys + seg->filesz) >> get_page_shift(ctx);
		if (pfn > get_max_pfn(ctx))
			set_max_pfn(ctx, pfn);

		if (ctx->shared->arch_ops && ctx->shared->arch_ops->process_load) {
			ret = ctx->shared->arch_ops->process_load(
				ctx, seg->virt, seg->phys);
			if (ret != KDUMP_OK)
				return ret;
		}
	}

	for (i = 0; i < edp->num_sections; ++i) {
		struct section *sect = edp->sections + i;
		const char *name = strtab_entry(edp, sect->name_index);
		if (!strcmp(name, ".xen_pages"))
			edp->xen_pages_offset = sect->file_offset;
		else if (!strcmp(name, ".xen_p2m")) {
			ctx->shared->xen_map = read_elf_sect(ctx, sect);
			if (!ctx->shared->xen_map)
				return KDUMP_ERR_SYSTEM;
			ctx->shared->xen_map_size = sect->size /sizeof(struct xen_p2m);
			set_xen_xlat(ctx, KDUMP_XEN_NONAUTO);
			get_max_pfn_xen_nonauto(ctx);
		} else if (!strcmp(name, ".xen_pfn")) {
			ctx->shared->xen_map = read_elf_sect(ctx, sect);
			if (!ctx->shared->xen_map)
				return KDUMP_ERR_SYSTEM;
			ctx->shared->xen_map_size = sect->size / sizeof(uint64_t);
			set_xen_xlat(ctx, KDUMP_XEN_AUTO);
			get_max_pfn_xen_auto(ctx);
		} else if (!strcmp(name, ".note.Xen")) {
			void *notes = read_elf_sect(ctx, sect);
			if (!notes)
				return KDUMP_ERR_SYSTEM;
			ret = process_notes(ctx, notes, sect->size);
			free(notes);
			if (ret != KDUMP_OK)
				return set_error(ctx, ret,
						 "Cannot process Xen notes");
		} else if (!strcmp(name, ".xen_prstatus")) {
			void *data = read_elf_sect(ctx, sect);
			if (!data)
				return KDUMP_ERR_SYSTEM;
			ret = ctx->shared->arch_ops->process_xen_prstatus(
				ctx, data, sect->size);
			free(data);
			if (ret != KDUMP_OK)
				return set_error(ctx, ret,
						 "Cannot process Xen prstatus");
		}
	}

	if (edp->xen_pages_offset) {
		set_xen_type(ctx, KDUMP_XEN_DOMAIN);
		if (!ctx->shared->xen_map)
			return set_error(ctx, KDUMP_ERR_NOTIMPL,
					 "Missing Xen P2M mapping");
		ctx->shared->ops = &xc_core_elf_ops;
		set_addrspace_caps(ctx->shared,
				   ADDRXLAT_CAPS(ADDRXLAT_KPHYSADDR) |
				   ADDRXLAT_CAPS(ADDRXLAT_MACHPHYSADDR));
	}

	return KDUMP_OK;
}

static kdump_status
do_probe(kdump_ctx_t *ctx, void *hdr)
{
	unsigned char *eheader = hdr;
	Elf32_Ehdr *elf32 = hdr;
	Elf64_Ehdr *elf64 = hdr;
	struct elfdump_priv *edp;

	if (memcmp(eheader, ELFMAG, SELFMAG))
		return set_error(ctx, KDUMP_NOPROBE,
				 "Invalid ELF signature");

	edp = calloc(1, sizeof *edp);
	if (!edp)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot allocate ELF dump private data");
	ctx->shared->fmtdata = edp;

	set_addrspace_caps(ctx->shared,
			   ADDRXLAT_CAPS(ADDRXLAT_MACHPHYSADDR));

	switch (eheader[EI_DATA]) {
	case ELFDATA2LSB:
		set_byte_order(ctx, KDUMP_LITTLE_ENDIAN);
		break;
	case ELFDATA2MSB:
		set_byte_order(ctx, KDUMP_BIG_ENDIAN);
		break;
	default:
		return set_error(ctx, KDUMP_ERR_NOTIMPL,
				 "Unsupported ELF data format: %u",
				 eheader[EI_DATA]);
	}

        if ((elf32->e_ident[EI_CLASS] == ELFCLASS32) &&
	    (dump16toh(ctx, elf32->e_type) == ET_CORE) &&
	    (dump32toh(ctx, elf32->e_version) == EV_CURRENT)) {
		edp->elfclass = ELFCLASS32;
		set_file_description(ctx, "ELF32 dump");
		return init_elf32(ctx, elf32);
	} else if ((elf64->e_ident[EI_CLASS] == ELFCLASS64) &&
		   (dump16toh(ctx, elf64->e_type) == ET_CORE) &&
		   (dump32toh(ctx, elf64->e_version) == EV_CURRENT)) {
		edp->elfclass = ELFCLASS64;
		set_file_description(ctx, "ELF64 dump");
		return init_elf64(ctx, elf64);
	}

	return set_error(ctx, KDUMP_ERR_NOTIMPL,
			 "Unsupported ELF class: %u", elf32->e_ident[EI_CLASS]);
}

static kdump_status
elf_probe(kdump_ctx_t *ctx)
{
	struct fcache_chunk fch;
	kdump_status ret;

	ret = fcache_get_chunk(ctx->shared->fcache, &fch,
			       sizeof(Elf64_Ehdr), 0);
	if (ret != KDUMP_OK)
		return set_error(ctx, ret, "Cannot read dump header");

	ret = do_probe(ctx, fch.data);
	fcache_put_chunk(&fch);

	if (ret == KDUMP_OK)
		ret = open_common(ctx);

	if (ret != KDUMP_OK)
		elf_cleanup(ctx->shared);

	return ret;
}

static void
elf_cleanup(struct kdump_shared *shared)
{
	struct elfdump_priv *edp = shared->fmtdata;

	if (edp) {
		if (edp->load_segments)
			free(edp->load_segments);
		if (edp->sections)
			free(edp->sections);
		if (edp->strtab)
			free(edp->strtab);
		free(edp);
		shared->fmtdata = NULL;
	}
};

const struct format_ops elfdump_ops = {
	.name = "elf",
	.probe = elf_probe,
	.get_page = elf_get_page,
	.put_page = cache_put_page,
	.realloc_caches = def_realloc_caches,
	.cleanup = elf_cleanup,
};

static const struct format_ops xc_core_elf_ops = {
	.name = "xc_core_elf",
	.get_page = xc_get_page,
	.put_page = cache_put_page,
	.post_addrxlat = xc_post_addrxlat,
	.realloc_caches = def_realloc_caches,
	.cleanup = elf_cleanup,
};
