/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *      Jerome Glisse
 */
#include "r600_pipe.h"
#include "evergreend.h"
#include "util/u_memory.h"
#include "util/u_math.h"

void evergreen_dma_copy(struct r600_context *rctx,
		struct pipe_resource *dst,
		struct pipe_resource *src,
		uint64_t dst_offset,
		uint64_t src_offset,
		uint64_t size)
{
	struct radeon_winsys_cs *cs = rctx->b.rings.dma.cs;
	unsigned i, ncopy, csize, sub_cmd, shift;
	struct r600_resource *rdst = (struct r600_resource*)dst;
	struct r600_resource *rsrc = (struct r600_resource*)src;

	/* make sure that the dma ring is only one active */
	rctx->b.rings.gfx.flush(rctx, RADEON_FLUSH_ASYNC);
	dst_offset += r600_resource_va(&rctx->screen->b.b, dst);
	src_offset += r600_resource_va(&rctx->screen->b.b, src);

	/* see if we use dword or byte copy */
	if (!(dst_offset & 0x3) && !(src_offset & 0x3) && !(size & 0x3)) {
		size >>= 2;
		sub_cmd = 0x00;
		shift = 2;
	} else {
		sub_cmd = 0x40;
		shift = 0;
	}
	ncopy = (size / 0x000fffff) + !!(size % 0x000fffff);

	r600_need_dma_space(rctx, ncopy * 5);
	for (i = 0; i < ncopy; i++) {
		csize = size < 0x000fffff ? size : 0x000fffff;
		/* emit reloc before writting cs so that cs is always in consistent state */
		r600_context_bo_reloc(&rctx->b, &rctx->b.rings.dma, rsrc, RADEON_USAGE_READ);
		r600_context_bo_reloc(&rctx->b, &rctx->b.rings.dma, rdst, RADEON_USAGE_WRITE);
		cs->buf[cs->cdw++] = DMA_PACKET(DMA_PACKET_COPY, sub_cmd, csize);
		cs->buf[cs->cdw++] = dst_offset & 0xffffffff;
		cs->buf[cs->cdw++] = src_offset & 0xffffffff;
		cs->buf[cs->cdw++] = (dst_offset >> 32UL) & 0xff;
		cs->buf[cs->cdw++] = (src_offset >> 32UL) & 0xff;
		dst_offset += csize << shift;
		src_offset += csize << shift;
		size -= csize;
	}

	util_range_add(&rdst->valid_buffer_range, dst_offset,
		       dst_offset + size);
}

/* The max number of bytes to copy per packet. */
#define CP_DMA_MAX_BYTE_COUNT ((1 << 21) - 8)

void evergreen_cp_dma_clear_buffer(struct r600_context *rctx,
				   struct pipe_resource *dst, uint64_t offset,
				   unsigned size, uint32_t clear_value)
{
	struct radeon_winsys_cs *cs = rctx->b.rings.gfx.cs;

	assert(size);
	assert(rctx->screen->has_cp_dma);

	offset += r600_resource_va(&rctx->screen->b.b, dst);

	/* Flush the cache where the resource is bound. */
	r600_flag_resource_cache_flush(rctx, dst);
        rctx->b.flags |= R600_CONTEXT_WAIT_3D_IDLE;

	while (size) {
		unsigned sync = 0;
		unsigned byte_count = MIN2(size, CP_DMA_MAX_BYTE_COUNT);
		unsigned reloc;

		r600_need_cs_space(rctx, 10 + (rctx->b.flags ? R600_MAX_FLUSH_CS_DWORDS : 0), FALSE);

		/* Flush the caches for the first copy only. */
		if (rctx->b.flags) {
			r600_flush_emit(rctx);
		}

		/* Do the synchronization after the last copy, so that all data is written to memory. */
		if (size == byte_count) {
			sync = PKT3_CP_DMA_CP_SYNC;
		}

		/* This must be done after r600_need_cs_space. */
		reloc = r600_context_bo_reloc(&rctx->b, &rctx->b.rings.gfx,
					      (struct r600_resource*)dst, RADEON_USAGE_WRITE);

		radeon_emit(cs, PKT3(PKT3_CP_DMA, 4, 0));
		radeon_emit(cs, clear_value);	/* DATA [31:0] */
		radeon_emit(cs, sync | PKT3_CP_DMA_SRC_SEL(2));	/* CP_SYNC [31] | SRC_SEL[30:29] */
		radeon_emit(cs, offset);	/* DST_ADDR_LO [31:0] */
		radeon_emit(cs, (offset >> 32) & 0xff);		/* DST_ADDR_HI [7:0] */
		radeon_emit(cs, byte_count);	/* COMMAND [29:22] | BYTE_COUNT [20:0] */

		radeon_emit(cs, PKT3(PKT3_NOP, 0, 0));
		radeon_emit(cs, reloc);

		size -= byte_count;
		offset += byte_count;
	}

	/* Flush the cache again in case the 3D engine has been prefetching
	 * the resource. */
	r600_flag_resource_cache_flush(rctx, dst);

	util_range_add(&r600_resource(dst)->valid_buffer_range, offset,
		       offset + size);
}

