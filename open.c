/* Routines for opening dumps.
   Copyright (C) 2014 Petr Tesarik <ptesarik@suse.cz>

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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kdumpfile-priv.h"

static kdump_status kdump_open_known(kdump_ctx *pctx);


static kdump_status
kvm_probe(kdump_ctx *ctx)
{
	static const char magic[] =
		{ 'Q', 'E', 'V', 'M' };

	if (memcmp(ctx->buffer, magic, sizeof magic))
		return kdump_unsupported;

	/* KVM dump not yet implemented */
	ctx->format = "KVM";
	return kdump_unsupported;
}

const struct kdump_ops kdump_kvm_ops = {
	.probe = kvm_probe,
};

static kdump_status
libvirt_probe(kdump_ctx *ctx)
{
	static const char magic[] =
		{ 'L', 'i', 'b', 'v' };

	if (memcmp(ctx->buffer, magic, sizeof magic))
		return kdump_unsupported;

	/* Libvirt dump not yet implemented */
	ctx->format = "Libvirt";
	return kdump_unsupported;
}

const struct kdump_ops kdump_libvirt_ops = {
	.probe = libvirt_probe,
};

static kdump_status
xc_save_probe(kdump_ctx *ctx)
{
	static const char magic[] =
		{ 'L', 'i', 'n', 'u', 'x', 'G', 'u', 'e',
		  's', 't', 'R', 'e', 'c', 'o', 'r', 'd' };

	if (memcmp(ctx->buffer, magic, sizeof magic))
		return kdump_unsupported;

	/* Xen xc_save not yet implemented */
	ctx->format = "Xen xc_save";
	return kdump_unsupported;
}

const struct kdump_ops kdump_xc_save_ops = {
	.probe = xc_save_probe,
};

static kdump_status
xc_core_probe(kdump_ctx *ctx)
{
	static const char magic[] =
		{ 0xeb, 0x0f, 0xf0 };
	unsigned char firstbyte;

	if (memcmp(ctx->buffer + 1, magic, sizeof magic))
		return kdump_unsupported;

	firstbyte = *(unsigned char*)ctx->buffer;
	if (firstbyte == 0xed)
		ctx->format = "Xen xc_core";
	else if (firstbyte = 0xee)
		ctx->format = "Xen xc_core hvm";
	else
		return kdump_unsupported;

	/* Xen xc_core not yet implemented */
	return kdump_unsupported;
}

const struct kdump_ops kdump_xc_core_ops = {
	.probe = xc_core_probe,
};

static kdump_status
mclxcd_probe(kdump_ctx *ctx)
{
	static const char magic[] =
		{ 0xdd, 0xcc, 0x8b, 0x9a };

	if (memcmp(ctx->buffer, magic, sizeof magic))
		return kdump_unsupported;

	/* MCLXCD dump not yet implemented */
	ctx->format = "MCLXCD";
	return kdump_unsupported;
}

const struct kdump_ops kdump_mclxcd_ops = {
	.probe = mclxcd_probe,
};

static kdump_status
s390_probe(kdump_ctx *ctx)
{
	static const char magic[] =
		{ 0xa8, 0x19, 0x01, 0x73, 0x61, 0x8f, 0x23, 0xfd };

	if (memcmp(ctx->buffer, magic, sizeof magic))
		return kdump_unsupported;

	/* S/390 dump not yet implemented */
	ctx->format = "S390";
	return kdump_unsupported;
}

const struct kdump_ops kdump_s390_ops = {
	.probe = s390_probe,
};

static const struct kdump_ops *formats[] = {
	&kdump_elfdump_ops,
	&kdump_kvm_ops,
	&kdump_libvirt_ops,
	&kdump_xc_save_ops,
	&kdump_xc_core_ops,
	&kdump_diskdump_ops,
	&kdump_lkcd_ops,
	&kdump_mclxcd_ops,
	&kdump_s390_ops,
	&kdump_devmem_ops
};

#define NFORMATS	(sizeof formats / sizeof formats[0])

/* /dev/crash cannot handle reads larger than page size */
static int
paged_cpin(int fd, void *buffer, size_t size)
{
	long page_size = sysconf(_SC_PAGESIZE);
	while (size) {
		size_t chunksize = (size > page_size)
			? page_size
			: size;
		if (read(fd, buffer, chunksize) != chunksize)
			return size;

		buffer += chunksize;
		size -= chunksize;
	}
	return 0;
}

kdump_status
kdump_fdopen(kdump_ctx **pctx, int fd)
{
	kdump_ctx *ctx;
	kdump_status ret;
	int i;

	ret = kdump_syserr;

	/* Initialize context */
	ctx = calloc(1, sizeof *ctx);
	if (!ctx)
		goto err;

	ctx->buffer = malloc(MAX_PAGE_SIZE);
	if (!ctx->buffer)
		goto err_ctx;

	ctx->fd = fd;

	if (paged_cpin(ctx->fd, ctx->buffer, MAX_PAGE_SIZE))
		goto err_ctx;

	for (i = 0; i < NFORMATS; ++i) {
		ctx->ops = formats[i];
		ret = ctx->ops->probe(ctx);
		if (ret == kdump_ok) {
			*pctx = ctx;
			return kdump_open_known(ctx);
		}
	}

  err_ctx:
	kdump_free(ctx);
  err:
	return ret;
}

static kdump_status
kdump_open_known(kdump_ctx *ctx)
{
	ctx->page = malloc(ctx->page_size);
	if (!ctx->page) {
		kdump_free(ctx);
		return kdump_syserr;
	}

	return kdump_ok;
}

void
kdump_free(kdump_ctx *ctx)
{
	if (ctx->ops && ctx->ops->free)
		ctx->ops->free(ctx);
	if (ctx->page)
		free(ctx->page);
	if (ctx->buffer)
		free(ctx->buffer);
	free(ctx);
}
