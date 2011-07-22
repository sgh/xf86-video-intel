/*
 * Copyright © 2006,2008,2011 Intel Corporation
 * Copyright © 2007 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Wang Zhenyu <zhenyu.z.wang@sna.com>
 *    Eric Anholt <eric@anholt.net>
 *    Carl Worth <cworth@redhat.com>
 *    Keith Packard <keithp@keithp.com>
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xf86.h>

#include "sna.h"
#include "sna_reg.h"
#include "sna_render.h"
#include "sna_render_inline.h"
#include "sna_video.h"

#include "gen7_render.h"

#if DEBUG_RENDER
#undef DBG
#define DBG(x) ErrorF x
#else
#define NDEBUG 1
#endif

#define NO_COMPOSITE 0
#define NO_COPY 0
#define NO_COPY_BOXES 0
#define NO_FILL 0
#define NO_FILL_BOXES 0

#define GEN7_MAX_SIZE 16384

/* XXX Todo
 *
 * STR (software tiled rendering) mode. No, really.
 * 64x32 pixel blocks align with the rendering cache. Worth considering.
 */

#define is_aligned(x, y) (((x) & ((y) - 1)) == 0)

static const uint32_t ps_kernel_nomask_affine[][4] = {
#include "exa_wm_src_affine.g7b"
#include "exa_wm_src_sample_argb.g7b"
#include "exa_wm_write.g7b"
};

static const uint32_t ps_kernel_nomask_projective[][4] = {
#include "exa_wm_src_projective.g7b"
#include "exa_wm_src_sample_argb.g7b"
#include "exa_wm_write.g7b"
};

static const uint32_t ps_kernel_maskca_affine[][4] = {
#include "exa_wm_src_affine.g7b"
#include "exa_wm_src_sample_argb.g7b"
#include "exa_wm_mask_affine.g7b"
#include "exa_wm_mask_sample_argb.g7b"
#include "exa_wm_ca.g6b" //#include "exa_wm_ca.g7b"
#include "exa_wm_write.g7b"
};

static const uint32_t ps_kernel_maskca_projective[][4] = {
#include "exa_wm_src_projective.g7b"
#include "exa_wm_src_sample_argb.g7b"
#include "exa_wm_mask_projective.g7b"
#include "exa_wm_mask_sample_argb.g7b"
#include "exa_wm_ca.g6b" //#include "exa_wm_ca.g7b"
#include "exa_wm_write.g7b"
};

static const uint32_t ps_kernel_maskca_srcalpha_affine[][4] = {
#include "exa_wm_src_affine.g7b"
#include "exa_wm_src_sample_a.g7b"
#include "exa_wm_mask_affine.g7b"
#include "exa_wm_mask_sample_argb.g7b"
#include "exa_wm_ca_srcalpha.g6b" //#include "exa_wm_ca_srcalpha.g7b"
#include "exa_wm_write.g7b"
};

static const uint32_t ps_kernel_maskca_srcalpha_projective[][4] = {
#include "exa_wm_src_projective.g7b"
#include "exa_wm_src_sample_a.g7b"
#include "exa_wm_mask_projective.g7b"
#include "exa_wm_mask_sample_argb.g7b"
#include "exa_wm_ca_srcalpha.g6b" //#include "exa_wm_ca_srcalpha.g7b"
#include "exa_wm_write.g7b"
};

static const uint32_t ps_kernel_masknoca_affine[][4] = {
#include "exa_wm_src_affine.g7b"
#include "exa_wm_src_sample_argb.g7b"
#include "exa_wm_mask_affine.g7b"
#include "exa_wm_mask_sample_a.g7b"
#include "exa_wm_noca.g6b"// #include "exa_wm_noca.g7b"
#include "exa_wm_write.g7b"
};

static const uint32_t ps_kernel_masknoca_projective[][4] = {
#include "exa_wm_src_projective.g7b"
#include "exa_wm_src_sample_argb.g7b"
#include "exa_wm_mask_projective.g7b"
#include "exa_wm_mask_sample_a.g7b"
#include "exa_wm_noca.g6b" //#include "exa_wm_noca.g7b"
#include "exa_wm_write.g7b"
};

static const uint32_t ps_kernel_packed[][4] = {
#include "exa_wm_src_affine.g7b"
#include "exa_wm_src_sample_argb.g7b"
#include "exa_wm_yuv_rgb.g7b"
#include "exa_wm_write.g7b"
};

static const uint32_t ps_kernel_planar[][4] = {
#include "exa_wm_src_affine.g7b"
#include "exa_wm_src_sample_planar.g7b"
#include "exa_wm_yuv_rgb.g7b"
#include "exa_wm_write.g7b"
};

#define KERNEL(kernel_enum, kernel, masked) \
    [GEN7_WM_KERNEL_##kernel_enum] = {#kernel_enum, kernel, sizeof(kernel), masked}
static const struct wm_kernel_info {
	const char *name;
	const void *data;
	unsigned int size;
	Bool has_mask;
} wm_kernels[] = {
	KERNEL(NOMASK, ps_kernel_nomask_affine, FALSE),
#if 0
	KERNEL(NOMASK_PROJECTIVE, ps_kernel_nomask_projective, FALSE),

	KERNEL(MASK, ps_kernel_masknoca_affine, TRUE),
	KERNEL(MASK_PROJECTIVE, ps_kernel_masknoca_projective, TRUE),

	KERNEL(MASKCA, ps_kernel_maskca_affine, TRUE),
	KERNEL(MASKCA_PROJECTIVE, ps_kernel_maskca_projective, TRUE),

	KERNEL(MASKCA_SRCALPHA, ps_kernel_maskca_srcalpha_affine, TRUE),
	KERNEL(MASKCA_SRCALPHA_PROJECTIVE, ps_kernel_maskca_srcalpha_projective, TRUE),
#endif

	KERNEL(VIDEO_PLANAR, ps_kernel_planar, FALSE),
	KERNEL(VIDEO_PACKED, ps_kernel_packed, FALSE),
};
#undef KERNEL

static const struct blendinfo {
	Bool src_alpha;
	uint32_t src_blend;
	uint32_t dst_blend;
} gen7_blend_op[] = {
	/* Clear */	{0, GEN7_BLENDFACTOR_ZERO, GEN7_BLENDFACTOR_ZERO},
	/* Src */	{0, GEN7_BLENDFACTOR_ONE, GEN7_BLENDFACTOR_ZERO},
	/* Dst */	{0, GEN7_BLENDFACTOR_ZERO, GEN7_BLENDFACTOR_ONE},
	/* Over */	{1, GEN7_BLENDFACTOR_ONE, GEN7_BLENDFACTOR_INV_SRC_ALPHA},
	/* OverReverse */ {0, GEN7_BLENDFACTOR_INV_DST_ALPHA, GEN7_BLENDFACTOR_ONE},
	/* In */	{0, GEN7_BLENDFACTOR_DST_ALPHA, GEN7_BLENDFACTOR_ZERO},
	/* InReverse */	{1, GEN7_BLENDFACTOR_ZERO, GEN7_BLENDFACTOR_SRC_ALPHA},
	/* Out */	{0, GEN7_BLENDFACTOR_INV_DST_ALPHA, GEN7_BLENDFACTOR_ZERO},
	/* OutReverse */ {1, GEN7_BLENDFACTOR_ZERO, GEN7_BLENDFACTOR_INV_SRC_ALPHA},
	/* Atop */	{1, GEN7_BLENDFACTOR_DST_ALPHA, GEN7_BLENDFACTOR_INV_SRC_ALPHA},
	/* AtopReverse */ {1, GEN7_BLENDFACTOR_INV_DST_ALPHA, GEN7_BLENDFACTOR_SRC_ALPHA},
	/* Xor */	{1, GEN7_BLENDFACTOR_INV_DST_ALPHA, GEN7_BLENDFACTOR_INV_SRC_ALPHA},
	/* Add */	{0, GEN7_BLENDFACTOR_ONE, GEN7_BLENDFACTOR_ONE},
};

/**
 * Highest-valued BLENDFACTOR used in gen7_blend_op.
 *
 * This leaves out GEN7_BLENDFACTOR_INV_DST_COLOR,
 * GEN7_BLENDFACTOR_INV_CONST_{COLOR,ALPHA},
 * GEN7_BLENDFACTOR_INV_SRC1_{COLOR,ALPHA}
 */
#define GEN7_BLENDFACTOR_COUNT (GEN7_BLENDFACTOR_INV_DST_ALPHA + 1)

/* FIXME: surface format defined in gen7_defines.h, shared Sampling engine
 * 1.7.2
 */
static const struct formatinfo {
	CARD32 pict_fmt;
	uint32_t card_fmt;
} gen7_tex_formats[] = {
	{PICT_a8, GEN7_SURFACEFORMAT_A8_UNORM},
	{PICT_a8r8g8b8, GEN7_SURFACEFORMAT_B8G8R8A8_UNORM},
	{PICT_x8r8g8b8, GEN7_SURFACEFORMAT_B8G8R8X8_UNORM},
	{PICT_a8b8g8r8, GEN7_SURFACEFORMAT_R8G8B8A8_UNORM},
	{PICT_x8b8g8r8, GEN7_SURFACEFORMAT_R8G8B8X8_UNORM},
	{PICT_r8g8b8, GEN7_SURFACEFORMAT_R8G8B8_UNORM},
	{PICT_r5g6b5, GEN7_SURFACEFORMAT_B5G6R5_UNORM},
	{PICT_a1r5g5b5, GEN7_SURFACEFORMAT_B5G5R5A1_UNORM},
	{PICT_a2r10g10b10, GEN7_SURFACEFORMAT_B10G10R10A2_UNORM},
	{PICT_x2r10g10b10, GEN7_SURFACEFORMAT_B10G10R10X2_UNORM},
	{PICT_a2b10g10r10, GEN7_SURFACEFORMAT_R10G10B10A2_UNORM},
	{PICT_x2r10g10b10, GEN7_SURFACEFORMAT_B10G10R10X2_UNORM},
	{PICT_a4r4g4b4, GEN7_SURFACEFORMAT_B4G4R4A4_UNORM},
};

#define GEN7_BLEND_STATE_PADDED_SIZE	ALIGN(sizeof(struct gen7_blend_state), 64)

#define BLEND_OFFSET(s, d) \
	(((s) * GEN7_BLENDFACTOR_COUNT + (d)) * GEN7_BLEND_STATE_PADDED_SIZE)

#define SAMPLER_OFFSET(sf, se, mf, me) \
	(((((sf) * EXTEND_COUNT + (se)) * FILTER_COUNT + (mf)) * EXTEND_COUNT + (me)) * 2 * sizeof(struct gen7_sampler_state))

#define OUT_BATCH(v) batch_emit(sna, v)
#define OUT_VERTEX(x,y) vertex_emit_2s(sna, x,y)
#define OUT_VERTEX_F(v) vertex_emit(sna, v)

static uint32_t gen7_get_blend(int op,
			       Bool has_component_alpha,
			       uint32_t dst_format)
{
	uint32_t src, dst;

	src = gen7_blend_op[op].src_blend;
	dst = gen7_blend_op[op].dst_blend;

	/* If there's no dst alpha channel, adjust the blend op so that
	 * we'll treat it always as 1.
	 */
	if (PICT_FORMAT_A(dst_format) == 0) {
		if (src == GEN7_BLENDFACTOR_DST_ALPHA)
			src = GEN7_BLENDFACTOR_ONE;
		else if (src == GEN7_BLENDFACTOR_INV_DST_ALPHA)
			src = GEN7_BLENDFACTOR_ZERO;
	}

	/* If the source alpha is being used, then we should only be in a
	 * case where the source blend factor is 0, and the source blend
	 * value is the mask channels multiplied by the source picture's alpha.
	 */
	if (has_component_alpha && gen7_blend_op[op].src_alpha) {
		if (dst == GEN7_BLENDFACTOR_SRC_ALPHA)
			dst = GEN7_BLENDFACTOR_SRC_COLOR;
		else if (dst == GEN7_BLENDFACTOR_INV_SRC_ALPHA)
			dst = GEN7_BLENDFACTOR_INV_SRC_COLOR;
	}

	DBG(("blend op=%d, dst=%x [A=%d] => src=%d, dst=%d => offset=%x\n",
	     op, dst_format, PICT_FORMAT_A(dst_format),
	     src, dst, (int)BLEND_OFFSET(src, dst)));
	return BLEND_OFFSET(src, dst);
}

static uint32_t gen7_get_dest_format(PictFormat format)
{
	switch (format) {
	default:
		assert(0);
	case PICT_a8r8g8b8:
	case PICT_x8r8g8b8:
		return GEN7_SURFACEFORMAT_B8G8R8A8_UNORM;
	case PICT_a8b8g8r8:
	case PICT_x8b8g8r8:
		return GEN7_SURFACEFORMAT_R8G8B8A8_UNORM;
	case PICT_a2r10g10b10:
	case PICT_x2r10g10b10:
		return GEN7_SURFACEFORMAT_B10G10R10A2_UNORM;
	case PICT_r5g6b5:
		return GEN7_SURFACEFORMAT_B5G6R5_UNORM;
	case PICT_x1r5g5b5:
	case PICT_a1r5g5b5:
		return GEN7_SURFACEFORMAT_B5G5R5A1_UNORM;
	case PICT_a8:
		return GEN7_SURFACEFORMAT_A8_UNORM;
	case PICT_a4r4g4b4:
	case PICT_x4r4g4b4:
		return GEN7_SURFACEFORMAT_B4G4R4A4_UNORM;
	}
}

static Bool gen7_check_dst_format(PictFormat format)
{
	switch (format) {
	case PICT_a8r8g8b8:
	case PICT_x8r8g8b8:
	case PICT_a8b8g8r8:
	case PICT_x8b8g8r8:
	case PICT_a2r10g10b10:
	case PICT_x2r10g10b10:
	case PICT_r5g6b5:
	case PICT_x1r5g5b5:
	case PICT_a1r5g5b5:
	case PICT_a8:
	case PICT_a4r4g4b4:
	case PICT_x4r4g4b4:
		return TRUE;
	}
	return FALSE;
}

static uint32_t gen7_get_dest_format_for_depth(int depth)
{
	switch (depth) {
	default: assert(0);
	case 32:
	case 24: return GEN7_SURFACEFORMAT_B8G8R8A8_UNORM;
	case 30: return GEN7_SURFACEFORMAT_B10G10R10A2_UNORM;
	case 16: return GEN7_SURFACEFORMAT_B5G6R5_UNORM;
	case 8:  return GEN7_SURFACEFORMAT_A8_UNORM;
	}
}

static uint32_t gen7_get_card_format_for_depth(int depth)
{
	switch (depth) {
	default: assert(0);
	case 32: return GEN7_SURFACEFORMAT_B8G8R8A8_UNORM;
	case 30: return GEN7_SURFACEFORMAT_B10G10R10A2_UNORM;
	case 24: return GEN7_SURFACEFORMAT_B8G8R8X8_UNORM;
	case 16: return GEN7_SURFACEFORMAT_B5G6R5_UNORM;
	case 8:  return GEN7_SURFACEFORMAT_A8_UNORM;
	}
}

static bool gen7_format_is_dst(uint32_t format)
{
	switch (format) {
	case GEN7_SURFACEFORMAT_B8G8R8A8_UNORM:
	case GEN7_SURFACEFORMAT_R8G8B8A8_UNORM:
	case GEN7_SURFACEFORMAT_B10G10R10A2_UNORM:
	case GEN7_SURFACEFORMAT_B5G6R5_UNORM:
	case GEN7_SURFACEFORMAT_B5G5R5A1_UNORM:
	case GEN7_SURFACEFORMAT_A8_UNORM:
	case GEN7_SURFACEFORMAT_B4G4R4A4_UNORM:
		return true;
	default:
		return false;
	}
}

static uint32_t gen7_filter(uint32_t filter)
{
	switch (filter) {
	default:
		assert(0);
	case PictFilterNearest:
		return SAMPLER_FILTER_NEAREST;
	case PictFilterBilinear:
		return SAMPLER_FILTER_BILINEAR;
	}
}

static uint32_t gen7_check_filter(PicturePtr picture)
{
	switch (picture->filter) {
	case PictFilterNearest:
	case PictFilterBilinear:
		return TRUE;
	default:
		return FALSE;
	}
}

static uint32_t gen7_repeat(uint32_t repeat)
{
	switch (repeat) {
	default:
		assert(0);
	case RepeatNone:
		return SAMPLER_EXTEND_NONE;
	case RepeatNormal:
		return SAMPLER_EXTEND_REPEAT;
	case RepeatPad:
		return SAMPLER_EXTEND_PAD;
	case RepeatReflect:
		return SAMPLER_EXTEND_REFLECT;
	}
}

static bool gen7_check_repeat(PicturePtr picture)
{
	if (!picture->repeat)
		return TRUE;

	switch (picture->repeatType) {
	case RepeatNone:
	case RepeatNormal:
	case RepeatPad:
	case RepeatReflect:
		return TRUE;
	default:
		return FALSE;
	}
}

static int
gen7_choose_composite_kernel(int op, Bool has_mask, Bool is_ca, Bool is_affine)
{
	int base;

	if (has_mask) {
		if (is_ca) {
			if (gen7_blend_op[op].src_alpha)
				base = GEN7_WM_KERNEL_MASKCA_SRCALPHA;
			else
				base = GEN7_WM_KERNEL_MASKCA;
		} else
			base = GEN7_WM_KERNEL_MASK;
	} else
		base = GEN7_WM_KERNEL_NOMASK;

	return base + !is_affine;
}

static void
gen7_emit_sip(struct sna *sna)
{
	/* Set system instruction pointer */
	OUT_BATCH(GEN7_STATE_SIP | 0);
	OUT_BATCH(0);
}

static void
gen7_emit_urb(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS | (2 - 2));
	OUT_BATCH(8); /* in 1KBs */

	/* num of VS entries must be divisible by 8 if size < 9 */
	OUT_BATCH(GEN7_3DSTATE_URB_VS | (2 - 2));
	OUT_BATCH((32 << GEN7_URB_ENTRY_NUMBER_SHIFT) | /* at least 32 */
		  (2 - 1) << GEN7_URB_ENTRY_SIZE_SHIFT |
		  (1 << GEN7_URB_STARTING_ADDRESS_SHIFT));

	OUT_BATCH(GEN7_3DSTATE_URB_HS | (2 - 2));
	OUT_BATCH((0 << GEN7_URB_ENTRY_SIZE_SHIFT) |
		  (2 << GEN7_URB_STARTING_ADDRESS_SHIFT));

	OUT_BATCH(GEN7_3DSTATE_URB_DS | (2 - 2));
	OUT_BATCH((0 << GEN7_URB_ENTRY_SIZE_SHIFT) |
		  (2 << GEN7_URB_STARTING_ADDRESS_SHIFT));

	OUT_BATCH(GEN7_3DSTATE_URB_GS | (2 - 2));
	OUT_BATCH((0 << GEN7_URB_ENTRY_SIZE_SHIFT) |
		  (1 << GEN7_URB_STARTING_ADDRESS_SHIFT));
}

static void
gen7_emit_state_base_address(struct sna *sna)
{
	OUT_BATCH(GEN7_STATE_BASE_ADDRESS | (10 - 2));
	OUT_BATCH(0); /* general */
	OUT_BATCH(kgem_add_reloc(&sna->kgem, /* surface */
				 sna->kgem.nbatch,
				 NULL,
				 I915_GEM_DOMAIN_INSTRUCTION << 16,
				 BASE_ADDRESS_MODIFY));
	OUT_BATCH(kgem_add_reloc(&sna->kgem, /* instruction */
				 sna->kgem.nbatch,
				 sna->render_state.gen7.general_bo,
				 I915_GEM_DOMAIN_INSTRUCTION << 16,
				 BASE_ADDRESS_MODIFY));
	OUT_BATCH(0); /* indirect */
	OUT_BATCH(kgem_add_reloc(&sna->kgem,
				 sna->kgem.nbatch,
				 sna->render_state.gen7.general_bo,
				 I915_GEM_DOMAIN_INSTRUCTION << 16,
				 BASE_ADDRESS_MODIFY));

	/* upper bounds, disable */
	OUT_BATCH(0);
	OUT_BATCH(BASE_ADDRESS_MODIFY);
	OUT_BATCH(0);
	OUT_BATCH(BASE_ADDRESS_MODIFY);
}

static void
gen7_disable_vs(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_VS | (6 - 2));
	OUT_BATCH(0); /* no VS kernel */
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0); /* pass-through */

#if 0
	OUT_BATCH(GEN7_3DSTATE_CONSTANT_VS | (7 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS | (2 - 2));
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS | (2 - 2));
	OUT_BATCH(0);
#endif
}

static void
gen7_disable_hs(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_HS | (7 - 2));
	OUT_BATCH(0); /* no HS kernel */
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0); /* pass-through */

#if 0
	OUT_BATCH(GEN7_3DSTATE_CONSTANT_HS | (7 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_BINDING_TABLE_POINTERS_HS | (2 - 2));
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_HS | (2 - 2));
	OUT_BATCH(0);
#endif
}

static void
gen7_disable_te(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_TE | (4 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen7_disable_ds(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_DS | (6 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

#if 0
	OUT_BATCH(GEN7_3DSTATE_CONSTANT_DS | (7 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_BINDING_TABLE_POINTERS_DS | (2 - 2));
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_DS | (2 - 2));
	OUT_BATCH(0);
#endif
}

static void
gen7_disable_gs(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_GS | (7 - 2));
	OUT_BATCH(0); /* no GS kernel */
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0); /* pass-through */

#if 0
	OUT_BATCH(GEN7_3DSTATE_CONSTANT_GS | (7 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_BINDING_TABLE_POINTERS_GS | (2 - 2));
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_GS | (2 - 2));
	OUT_BATCH(0);
#endif
}

static void
gen7_disable_streamout(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_STREAMOUT | (3 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen7_emit_sf_invariant(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_SF | (7 - 2));
	OUT_BATCH(0);
	OUT_BATCH(GEN7_3DSTATE_SF_CULL_NONE);
	OUT_BATCH(2 << GEN7_3DSTATE_SF_TRIFAN_PROVOKE_SHIFT);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen7_emit_cc_invariant(struct sna *sna)
{
#if 0 /* unused, no change */
	OUT_BATCH(GEN7_3DSTATE_CC_STATE_POINTERS | (2 - 2));
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_DEPTH_STENCIL_STATE_POINTERS | (2 - 2));
	OUT_BATCH(0);
#endif

	assert(is_aligned(sna->render_state.gen7.cc_vp, 32));
	OUT_BATCH(GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC | (2 - 2));
	OUT_BATCH(sna->render_state.gen7.cc_vp);
}

static void
gen7_disable_clip(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_CLIP | (4 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0); /* pass-through */
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CL | (2 - 2));
	OUT_BATCH(0);
}

static void
gen7_emit_wm_invariant(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_WM | (3 - 2));
	OUT_BATCH(GEN7_WM_DISPATCH_ENABLE |
		  GEN7_WM_PERSPECTIVE_PIXEL_BARYCENTRIC);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_CONSTANT_PS | (7 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen7_emit_null_depth_buffer(struct sna *sna)
{
	OUT_BATCH(GEN7_3DSTATE_DEPTH_BUFFER | (7 - 2));
	OUT_BATCH(GEN7_SURFACE_NULL << GEN7_3DSTATE_DEPTH_BUFFER_TYPE_SHIFT |
		  GEN7_DEPTHFORMAT_D32_FLOAT << GEN7_3DSTATE_DEPTH_BUFFER_FORMAT_SHIFT);
	OUT_BATCH(0); /* disable depth, stencil and hiz */
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_CLEAR_PARAMS | (3 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen7_emit_invariant(struct sna *sna)
{
	OUT_BATCH(GEN7_PIPELINE_SELECT | PIPELINE_SELECT_3D);

	OUT_BATCH(GEN7_3DSTATE_MULTISAMPLE | (4 - 2));
	OUT_BATCH(GEN7_3DSTATE_MULTISAMPLE_PIXEL_LOCATION_CENTER |
		  GEN7_3DSTATE_MULTISAMPLE_NUMSAMPLES_1); /* 1 sample/pixel */
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_SAMPLE_MASK | (2 - 2));
	OUT_BATCH(1);

	gen7_emit_sip(sna);
	gen7_emit_urb(sna);

	gen7_emit_state_base_address(sna);

	gen7_disable_vs(sna);
	gen7_disable_hs(sna);
	gen7_disable_te(sna);
	gen7_disable_ds(sna);
	gen7_disable_gs(sna);
	gen7_disable_clip(sna);
	gen7_emit_sf_invariant(sna);
	gen7_emit_wm_invariant(sna);
	gen7_emit_cc_invariant(sna);
	gen7_disable_streamout(sna);
	gen7_emit_null_depth_buffer(sna);

	sna->render_state.gen7.needs_invariant = FALSE;
}

static bool
gen7_emit_cc(struct sna *sna, uint32_t blend_offset)
{
	struct gen7_render_state *render = &sna->render_state.gen7;

	if (render->blend == blend_offset)
		return false;

	/* XXX can have upto 8 blend states preload, selectable via
	 * Render Target Index. What other side-effects of Render Target Index?
	 */

	assert (is_aligned(render->cc_blend + blend_offset, 64));
	OUT_BATCH(GEN7_3DSTATE_BLEND_STATE_POINTERS | (2 - 2));
	OUT_BATCH((render->cc_blend + blend_offset) | 1);

	render->blend = blend_offset;
	return true;
}

static void
gen7_emit_sampler(struct sna *sna, uint32_t state)
{
	assert(state <
	       2 * sizeof(struct gen7_sampler_state) *
	       FILTER_COUNT * EXTEND_COUNT *
	       FILTER_COUNT * EXTEND_COUNT);

	if (sna->render_state.gen7.samplers == state)
		return;

	sna->render_state.gen7.samplers = state;

	assert (is_aligned(sna->render_state.gen7.wm_state + state, 32));
	OUT_BATCH(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS | (2 - 2));
	OUT_BATCH(sna->render_state.gen7.wm_state + state);
}

static void
gen7_emit_sf(struct sna *sna, Bool has_mask)
{
	int num_sf_outputs = has_mask ? 2 : 1;

	if (sna->render_state.gen7.num_sf_outputs == num_sf_outputs)
		return;

	DBG(("%s: num_sf_outputs=%d, read_length=%d, read_offset=%d\n",
	     __FUNCTION__, num_sf_outputs, 1, 0));

	sna->render_state.gen7.num_sf_outputs = num_sf_outputs;

	OUT_BATCH(GEN7_3DSTATE_SBE | (14 - 2));
	OUT_BATCH(num_sf_outputs << GEN7_SBE_NUM_OUTPUTS_SHIFT |
		  1 << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT |
		  1 << GEN7_SBE_URB_ENTRY_READ_OFFSET_SHIFT);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0); /* dw4 */
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0); /* DW9 */
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen7_emit_wm(struct sna *sna, int kernel, int nr_surfaces, int nr_inputs)
{
	if (sna->render_state.gen7.kernel == kernel)
		return;

	sna->render_state.gen7.kernel = kernel;

	DBG(("%s: switching to %s\n", __FUNCTION__, wm_kernels[kernel].name));

	OUT_BATCH(GEN7_3DSTATE_PS | (8 - 2));
	OUT_BATCH(sna->render_state.gen7.wm_kernel[kernel]);
	OUT_BATCH(1 << GEN7_PS_SAMPLER_COUNT_SHIFT |
		  nr_surfaces << GEN7_PS_BINDING_TABLE_ENTRY_COUNT_SHIFT);
	OUT_BATCH(0); /* scratch address */
	OUT_BATCH((86 - 1) << GEN7_PS_MAX_THREADS_SHIFT |
		  GEN7_PS_ATTRIBUTE_ENABLE |
		  GEN7_PS_16_DISPATCH_ENABLE);
	OUT_BATCH(6 << GEN7_PS_DISPATCH_START_GRF_SHIFT_0);
	OUT_BATCH(0); /* kernel 1 */
	OUT_BATCH(0); /* kernel 2 */
}

static bool
gen7_emit_binding_table(struct sna *sna, uint16_t offset)
{
	if (sna->render_state.gen7.surface_table == offset)
		return false;

	/* Binding table pointers */
	assert(is_aligned(4*offset, 32));
	OUT_BATCH(GEN7_3DSTATE_BINDING_TABLE_POINTERS_PS | (2 - 2));
	OUT_BATCH(offset*4);

	sna->render_state.gen7.surface_table = offset;
	return true;
}

static void
gen7_emit_drawing_rectangle(struct sna *sna,
			    const struct sna_composite_op *op,
			    bool force)
{
	uint32_t limit = (op->dst.height - 1) << 16 | (op->dst.width - 1);
	uint32_t offset = (uint16_t)op->dst.y << 16 | (uint16_t)op->dst.x;

	if (!force &&
	    sna->render_state.gen7.drawrect_limit == limit &&
	    sna->render_state.gen7.drawrect_offset == offset)
		return;

	sna->render_state.gen7.drawrect_offset = offset;
	sna->render_state.gen7.drawrect_limit = limit;

	OUT_BATCH(GEN7_3DSTATE_DRAWING_RECTANGLE | (4 - 2));
	OUT_BATCH(0);
	OUT_BATCH(limit);
	OUT_BATCH(offset);
}

static void
gen7_emit_vertex_elements(struct sna *sna,
			  const struct sna_composite_op *op)
{
	/*
	 * vertex data in vertex buffer
	 *    position: (x, y)
	 *    texture coordinate 0: (u0, v0) if (is_affine is TRUE) else (u0, v0, w0)
	 *    texture coordinate 1 if (has_mask is TRUE): same as above
	 */
	struct gen7_render_state *render = &sna->render_state.gen7;
	int nelem = op->mask.bo ? 2 : 1;
	int selem = op->is_affine ? 2 : 3;
	uint32_t w_component;
	uint32_t src_format;
	int id = op->u.gen7.ve_id;

	if (render->ve_id == id)
		return;
	render->ve_id = id;

	if (op->is_affine) {
		src_format = GEN7_SURFACEFORMAT_R32G32_FLOAT;
		w_component = GEN7_VFCOMPONENT_STORE_1_FLT;
	} else {
		src_format = GEN7_SURFACEFORMAT_R32G32B32_FLOAT;
		w_component = GEN7_VFCOMPONENT_STORE_SRC;
	}

	/* The VUE layout
	 *    dword 0-3: pad (0.0, 0.0, 0.0. 0.0)
	 *    dword 4-7: position (x, y, 1.0, 1.0),
	 *    dword 8-11: texture coordinate 0 (u0, v0, w0, 1.0)
	 *    dword 12-15: texture coordinate 1 (u1, v1, w1, 1.0)
	 *
	 * dword 4-15 are fetched from vertex buffer
	 */
	OUT_BATCH(GEN7_3DSTATE_VERTEX_ELEMENTS |
		((2 * (2 + nelem)) + 1 - 2));

	OUT_BATCH(id << GEN7_VE0_VERTEX_BUFFER_INDEX_SHIFT |
		  GEN7_VE0_VALID |
		  GEN7_SURFACEFORMAT_R32G32B32A32_FLOAT << GEN7_VE0_FORMAT_SHIFT |
		  0 << GEN7_VE0_OFFSET_SHIFT);
	OUT_BATCH(GEN7_VFCOMPONENT_STORE_0 << GEN7_VE1_VFCOMPONENT_0_SHIFT |
		  GEN7_VFCOMPONENT_STORE_0 << GEN7_VE1_VFCOMPONENT_1_SHIFT |
		  GEN7_VFCOMPONENT_STORE_0 << GEN7_VE1_VFCOMPONENT_2_SHIFT |
		  GEN7_VFCOMPONENT_STORE_0 << GEN7_VE1_VFCOMPONENT_3_SHIFT);

	/* x,y */
	OUT_BATCH(id << GEN7_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN7_VE0_VALID |
		  GEN7_SURFACEFORMAT_R16G16_SSCALED << GEN7_VE0_FORMAT_SHIFT |
		  0 << GEN7_VE0_OFFSET_SHIFT); /* offsets vb in bytes */
	OUT_BATCH(GEN7_VFCOMPONENT_STORE_SRC << GEN7_VE1_VFCOMPONENT_0_SHIFT |
		  GEN7_VFCOMPONENT_STORE_SRC << GEN7_VE1_VFCOMPONENT_1_SHIFT |
		  GEN7_VFCOMPONENT_STORE_1_FLT << GEN7_VE1_VFCOMPONENT_2_SHIFT |
		  GEN7_VFCOMPONENT_STORE_1_FLT << GEN7_VE1_VFCOMPONENT_3_SHIFT);

	/* u0, v0, w0 */
	OUT_BATCH(id << GEN7_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN7_VE0_VALID |
		  src_format << GEN7_VE0_FORMAT_SHIFT |
		  4 << GEN7_VE0_OFFSET_SHIFT);	/* offset vb in bytes */
	OUT_BATCH(GEN7_VFCOMPONENT_STORE_SRC << GEN7_VE1_VFCOMPONENT_0_SHIFT |
		  GEN7_VFCOMPONENT_STORE_SRC << GEN7_VE1_VFCOMPONENT_1_SHIFT |
		  w_component << GEN7_VE1_VFCOMPONENT_2_SHIFT |
		  GEN7_VFCOMPONENT_STORE_1_FLT << GEN7_VE1_VFCOMPONENT_3_SHIFT);

	/* u1, v1, w1 */
	if (op->mask.bo) {
		OUT_BATCH(id << GEN7_VE0_VERTEX_BUFFER_INDEX_SHIFT |
			  GEN7_VE0_VALID |
			  src_format << GEN7_VE0_FORMAT_SHIFT |
			  ((1 + selem) * 4) << GEN7_VE0_OFFSET_SHIFT); /* vb offset in bytes */
		OUT_BATCH(GEN7_VFCOMPONENT_STORE_SRC << GEN7_VE1_VFCOMPONENT_0_SHIFT |
			  GEN7_VFCOMPONENT_STORE_SRC << GEN7_VE1_VFCOMPONENT_1_SHIFT |
			  w_component << GEN7_VE1_VFCOMPONENT_2_SHIFT |
			  GEN7_VFCOMPONENT_STORE_1_FLT << GEN7_VE1_VFCOMPONENT_3_SHIFT);
	}
}

static void
gen7_emit_state(struct sna *sna,
		const struct sna_composite_op *op,
		uint16_t wm_binding_table)

{
	bool flushed =
		(sna->kgem.batch[sna->kgem.nbatch-1] & (0xff<<23)) == MI_FLUSH;
	bool need_flush;

	need_flush = gen7_emit_cc(sna,
				  gen7_get_blend(op->op,
						 op->has_component_alpha,
						 op->dst.format));

	DBG(("%s: sampler src=(%d, %d), mask=(%d, %d), offset=%d\n",
	     __FUNCTION__,
	     op->src.filter, op->src.repeat,
	     op->mask.filter, op->mask.repeat,
	     (int)SAMPLER_OFFSET(op->src.filter,
				 op->src.repeat,
				 op->mask.filter,
				 op->mask.repeat)));
	gen7_emit_sampler(sna,
			  SAMPLER_OFFSET(op->src.filter,
					 op->src.repeat,
					 op->mask.filter,
					 op->mask.repeat));
	gen7_emit_sf(sna, op->mask.bo != NULL);
	gen7_emit_wm(sna,
		     op->u.gen7.wm_kernel,
		     op->u.gen7.nr_surfaces,
		     op->u.gen7.nr_inputs);
	gen7_emit_vertex_elements(sna, op);

	/* XXX updating the binding table requires a non-pipelined cmd? */
	need_flush |= gen7_emit_binding_table(sna, wm_binding_table);
	gen7_emit_drawing_rectangle(sna, op, need_flush & !flushed);
}

static void gen7_magic_ca_pass(struct sna *sna,
			       const struct sna_composite_op *op)
{
	struct gen7_render_state *state = &sna->render_state.gen7;
	bool need_flush;

	if (!op->need_magic_ca_pass)
		return;

	DBG(("%s: CA fixup (%d -> %d)\n", __FUNCTION__,
	     sna->render.vertex_start, sna->render.vertex_index));

	need_flush =
		gen7_emit_cc(sna,
			     gen7_get_blend(PictOpAdd, TRUE, op->dst.format));
	gen7_emit_wm(sna,
		     gen7_choose_composite_kernel(PictOpAdd,
						  TRUE, TRUE,
						  op->is_affine),
		     3, 2);

	/* XXX We apparently need a non-pipelined op to flush the
	 * pipeline before changing blend state.
	 */
	if (need_flush)
		OUT_BATCH(MI_FLUSH | MI_INHIBIT_RENDER_CACHE_FLUSH);

	OUT_BATCH(GEN7_3DPRIMITIVE | (7- 2));
	OUT_BATCH(GEN7_3DPRIMITIVE_VERTEX_SEQUENTIAL |
		  _3DPRIM_RECTLIST << GEN7_3DPRIMITIVE_TOPOLOGY_SHIFT);
	OUT_BATCH(sna->render.vertex_index - sna->render.vertex_start);
	OUT_BATCH(sna->render.vertex_start);
	OUT_BATCH(1);	/* single instance */
	OUT_BATCH(0);	/* start instance location */
	OUT_BATCH(0);	/* index buffer offset, ignored */

	state->last_primitive = sna->kgem.nbatch;
}

static void gen7_vertex_flush(struct sna *sna)
{
	if (sna->render_state.gen7.vertex_offset == 0)
		return;

	DBG(("%s[%x] = %d\n", __FUNCTION__,
	     4*sna->render_state.gen7.vertex_offset,
	     sna->render.vertex_index - sna->render.vertex_start));
	sna->kgem.batch[sna->render_state.gen7.vertex_offset] =
		sna->render.vertex_index - sna->render.vertex_start;
	sna->render_state.gen7.vertex_offset = 0;

	if (sna->render.op)
		gen7_magic_ca_pass(sna, sna->render.op);
}

static void gen7_vertex_finish(struct sna *sna, Bool last)
{
	struct kgem_bo *bo;
	int i, delta;

	gen7_vertex_flush(sna);
	if (!sna->render.vertex_used)
		return;

	/* Note: we only need dword alignment (currently) */

	if (last && sna->kgem.nbatch + sna->render.vertex_used <= sna->kgem.surface) {
		DBG(("%s: copy to batch: %d @ %d\n", __FUNCTION__,
		     sna->render.vertex_used, sna->kgem.nbatch));
		memcpy(sna->kgem.batch + sna->kgem.nbatch,
		       sna->render.vertex_data,
		       sna->render.vertex_used * 4);
		delta = sna->kgem.nbatch * 4;
		bo = NULL;
		sna->kgem.nbatch += sna->render.vertex_used;
	} else {
		bo = kgem_create_linear(&sna->kgem, 4*sna->render.vertex_used);
		if (bo && !kgem_bo_write(&sna->kgem, bo,
					 sna->render.vertex_data,
					 4*sna->render.vertex_used)) {
			kgem_bo_destroy(&sna->kgem, bo);
			return;
		}
		delta = 0;
		DBG(("%s: new vbo: %d\n", __FUNCTION__,
		     sna->render.vertex_used));
	}

	for (i = 0; i < ARRAY_SIZE(sna->render.vertex_reloc); i++) {
		if (sna->render.vertex_reloc[i]) {
			DBG(("%s: reloc[%d] = %d\n", __FUNCTION__,
			     i, sna->render.vertex_reloc[i]));

			sna->kgem.batch[sna->render.vertex_reloc[i]] =
				kgem_add_reloc(&sna->kgem,
					       sna->render.vertex_reloc[i],
					       bo,
					       I915_GEM_DOMAIN_VERTEX << 16,
					       delta);
			sna->kgem.batch[sna->render.vertex_reloc[i]+1] =
				kgem_add_reloc(&sna->kgem,
					       sna->render.vertex_reloc[i]+1,
					       bo,
					       I915_GEM_DOMAIN_VERTEX << 16,
					       delta + sna->render.vertex_used * 4 - 1);
			sna->render.vertex_reloc[i] = 0;
		}
	}

	if (bo)
		kgem_bo_destroy(&sna->kgem, bo);

	sna->render.vertex_used = 0;
	sna->render.vertex_index = 0;
	sna->render_state.gen7.vb_id = 0;
}

typedef struct gen7_surface_state_padded {
	struct gen7_surface_state state;
	char pad[32 - sizeof(struct gen7_surface_state)];
} gen7_surface_state_padded;

static void null_create(struct sna_static_stream *stream)
{
	/* A bunch of zeros useful for legacy border color and depth-stencil */
	sna_static_stream_map(stream, 64, 64);
}

static void
sampler_state_init(struct gen7_sampler_state *sampler_state,
		   sampler_filter_t filter,
		   sampler_extend_t extend)
{
	sampler_state->ss0.lod_preclamp = 1;	/* GL mode */

	/* We use the legacy mode to get the semantics specified by
	 * the Render extension. */
	sampler_state->ss0.default_color_mode = GEN7_BORDER_COLOR_MODE_LEGACY;

	switch (filter) {
	default:
	case SAMPLER_FILTER_NEAREST:
		sampler_state->ss0.min_filter = GEN7_MAPFILTER_NEAREST;
		sampler_state->ss0.mag_filter = GEN7_MAPFILTER_NEAREST;
		break;
	case SAMPLER_FILTER_BILINEAR:
		sampler_state->ss0.min_filter = GEN7_MAPFILTER_LINEAR;
		sampler_state->ss0.mag_filter = GEN7_MAPFILTER_LINEAR;
		break;
	}

	switch (extend) {
	default:
	case SAMPLER_EXTEND_NONE:
		sampler_state->ss3.r_wrap_mode = GEN7_TEXCOORDMODE_CLAMP_BORDER;
		sampler_state->ss3.s_wrap_mode = GEN7_TEXCOORDMODE_CLAMP_BORDER;
		sampler_state->ss3.t_wrap_mode = GEN7_TEXCOORDMODE_CLAMP_BORDER;
		break;
	case SAMPLER_EXTEND_REPEAT:
		sampler_state->ss3.r_wrap_mode = GEN7_TEXCOORDMODE_WRAP;
		sampler_state->ss3.s_wrap_mode = GEN7_TEXCOORDMODE_WRAP;
		sampler_state->ss3.t_wrap_mode = GEN7_TEXCOORDMODE_WRAP;
		break;
	case SAMPLER_EXTEND_PAD:
		sampler_state->ss3.r_wrap_mode = GEN7_TEXCOORDMODE_CLAMP;
		sampler_state->ss3.s_wrap_mode = GEN7_TEXCOORDMODE_CLAMP;
		sampler_state->ss3.t_wrap_mode = GEN7_TEXCOORDMODE_CLAMP;
		break;
	case SAMPLER_EXTEND_REFLECT:
		sampler_state->ss3.r_wrap_mode = GEN7_TEXCOORDMODE_MIRROR;
		sampler_state->ss3.s_wrap_mode = GEN7_TEXCOORDMODE_MIRROR;
		sampler_state->ss3.t_wrap_mode = GEN7_TEXCOORDMODE_MIRROR;
		break;
	}
}

static uint32_t gen7_create_cc_viewport(struct sna_static_stream *stream)
{
	struct gen7_cc_viewport vp;

	vp.min_depth = -1.e35;
	vp.max_depth = 1.e35;

	return sna_static_stream_add(stream, &vp, sizeof(vp), 32);
}

static uint32_t gen7_get_card_format(PictFormat format)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(gen7_tex_formats); i++) {
		if (gen7_tex_formats[i].pict_fmt == format)
			return gen7_tex_formats[i].card_fmt;
	}
	return -1;
}

static uint32_t
gen7_tiling_bits(uint32_t tiling)
{
	switch (tiling) {
	default: assert(0);
	case I915_TILING_NONE: return 0;
	case I915_TILING_X: return GEN7_SURFACE_TILED;
	case I915_TILING_Y: return GEN7_SURFACE_TILED | GEN7_SURFACE_TILED_Y;
	}
}

/**
 * Sets up the common fields for a surface state buffer for the given
 * picture in the given surface state buffer.
 */
static int
gen7_bind_bo(struct sna *sna,
	     struct kgem_bo *bo,
	     uint32_t width,
	     uint32_t height,
	     uint32_t format,
	     Bool is_dst)
{
	uint32_t *ss;
	uint32_t domains;
	uint16_t offset;

	/* After the first bind, we manage the cache domains within the batch */
	if (is_dst) {
		domains = I915_GEM_DOMAIN_RENDER << 16 |I915_GEM_DOMAIN_RENDER;
		kgem_bo_mark_dirty(bo);
	} else {
		domains = I915_GEM_DOMAIN_SAMPLER << 16;
		is_dst = gen7_format_is_dst(format);
	}

	offset = sna->kgem.surface - sizeof(struct gen7_surface_state_padded) / sizeof(uint32_t);
	offset *= sizeof(uint32_t);

	if (is_dst) {
		if (bo->dst_bound)
			return bo->dst_bound;

		bo->dst_bound = offset;
	} else {
		if (bo->src_bound)
			return bo->src_bound;

		bo->src_bound = offset;
	}

	sna->kgem.surface -=
		sizeof(struct gen7_surface_state_padded) / sizeof(uint32_t);
	ss = sna->kgem.batch + sna->kgem.surface;
	ss[0] = (GEN7_SURFACE_2D << GEN7_SURFACE_TYPE_SHIFT |
		 gen7_tiling_bits(bo->tiling) |
		 format << GEN7_SURFACE_FORMAT_SHIFT);
	ss[1] = kgem_add_reloc(&sna->kgem,
			       sna->kgem.surface + 1,
			       bo, domains, 0);
	ss[2] = ((width - 1)  << GEN7_SURFACE_WIDTH_SHIFT |
		 (height - 1) << GEN7_SURFACE_HEIGHT_SHIFT);
	ss[3] = (bo->pitch - 1) << GEN7_SURFACE_PITCH_SHIFT;
	ss[4] = 0;
	ss[5] = 0;
	ss[6] = 0;
	ss[7] = 0;

	DBG(("[%x] bind bo(handle=%d, addr=%d), format=%d, width=%d, height=%d, pitch=%d, tiling=%d -> %s\n",
	     offset, bo->handle, ss[1],
	     format, width, height, bo->pitch, bo->tiling,
	     domains & 0xffff ? "render" : "sampler"));

	return offset;
}

fastcall static void
gen7_emit_composite_primitive_solid(struct sna *sna,
				    const struct sna_composite_op *op,
				    const struct sna_composite_rectangles *r)
{
	float *v;
	union {
		struct sna_coordinate p;
		float f;
	} dst;

	v = sna->render.vertex_data + sna->render.vertex_used;
	sna->render.vertex_used += 9;

	dst.p.x = r->dst.x + r->width;
	dst.p.y = r->dst.y + r->height;
	v[0] = dst.f;
	v[1] = 1.;
	v[2] = 1.;

	dst.p.x = r->dst.x;
	v[3] = dst.f;
	v[4] = 0.;
	v[5] = 1.;

	dst.p.y = r->dst.y;
	v[6] = dst.f;
	v[7] = 0.;
	v[8] = 0.;
}

fastcall static void
gen7_emit_composite_primitive_identity_source(struct sna *sna,
					      const struct sna_composite_op *op,
					      const struct sna_composite_rectangles *r)
{
	union {
		struct sna_coordinate p;
		float f;
	} dst;
	float *v;

	v = sna->render.vertex_data + sna->render.vertex_used;
	sna->render.vertex_used += 9;

	dst.p.x = r->dst.x + r->width;
	dst.p.y = r->dst.y + r->height;
	v[0] = dst.f;
	dst.p.x = r->dst.x;
	v[3] = dst.f;
	dst.p.y = r->dst.y;
	v[6] = dst.f;

	v[7] = v[4] = (r->src.x + op->src.offset[0]) * op->src.scale[0];
	v[1] = v[4] + r->width * op->src.scale[0];

	v[8] = (r->src.y + op->src.offset[1]) * op->src.scale[1];
	v[5] = v[2] = v[8] + r->height * op->src.scale[1];
}

fastcall static void
gen7_emit_composite_primitive_affine_source(struct sna *sna,
					    const struct sna_composite_op *op,
					    const struct sna_composite_rectangles *r)
{
	union {
		struct sna_coordinate p;
		float f;
	} dst;
	float *v;

	v = sna->render.vertex_data + sna->render.vertex_used;
	sna->render.vertex_used += 9;

	dst.p.x = r->dst.x + r->width;
	dst.p.y = r->dst.y + r->height;
	v[0] = dst.f;
	_sna_get_transformed_coordinates(op->src.offset[0] + r->src.x + r->width,
					 op->src.offset[1] + r->src.y + r->height,
					 op->src.transform,
					 &v[1], &v[2]);
	v[1] *= op->src.scale[0];
	v[2] *= op->src.scale[1];

	dst.p.x = r->dst.x;
	v[3] = dst.f;
	_sna_get_transformed_coordinates(op->src.offset[0] + r->src.x,
					 op->src.offset[1] + r->src.y + r->height,
					 op->src.transform,
					 &v[4], &v[5]);
	v[4] *= op->src.scale[0];
	v[5] *= op->src.scale[1];

	dst.p.y = r->dst.y;
	v[6] = dst.f;
	_sna_get_transformed_coordinates(op->src.offset[0] + r->src.x,
					 op->src.offset[1] + r->src.y,
					 op->src.transform,
					 &v[7], &v[8]);
	v[7] *= op->src.scale[0];
	v[8] *= op->src.scale[1];
}

fastcall static void
gen7_emit_composite_primitive_identity_source_mask(struct sna *sna,
						   const struct sna_composite_op *op,
						   const struct sna_composite_rectangles *r)
{
	union {
		struct sna_coordinate p;
		float f;
	} dst;
	float src_x, src_y;
	float msk_x, msk_y;
	float w, h;
	float *v;

	src_x = r->src.x + op->src.offset[0];
	src_y = r->src.y + op->src.offset[1];
	msk_x = r->mask.x + op->mask.offset[0];
	msk_y = r->mask.y + op->mask.offset[1];
	w = r->width;
	h = r->height;

	v = sna->render.vertex_data + sna->render.vertex_used;
	sna->render.vertex_used += 15;

	dst.p.x = r->dst.x + r->width;
	dst.p.y = r->dst.y + r->height;
	v[0] = dst.f;
	v[1] = (src_x + w) * op->src.scale[0];
	v[2] = (src_y + h) * op->src.scale[1];
	v[3] = (msk_x + w) * op->mask.scale[0];
	v[4] = (msk_y + h) * op->mask.scale[1];

	dst.p.x = r->dst.x;
	v[5] = dst.f;
	v[6] = src_x * op->src.scale[0];
	v[7] = v[2];
	v[8] = msk_x * op->mask.scale[0];
	v[9] = v[4];

	dst.p.y = r->dst.y;
	v[10] = dst.f;
	v[11] = v[6];
	v[12] = src_y * op->src.scale[1];
	v[13] = v[8];
	v[14] = msk_y * op->mask.scale[1];
}

fastcall static void
gen7_emit_composite_primitive(struct sna *sna,
			      const struct sna_composite_op *op,
			      const struct sna_composite_rectangles *r)
{
	float src_x[3], src_y[3], src_w[3], mask_x[3], mask_y[3], mask_w[3];
	Bool is_affine = op->is_affine;
	const float *src_sf = op->src.scale;
	const float *mask_sf = op->mask.scale;

	if (is_affine) {
		sna_get_transformed_coordinates(r->src.x + op->src.offset[0],
						r->src.y + op->src.offset[1],
						op->src.transform,
						&src_x[0],
						&src_y[0]);

		sna_get_transformed_coordinates(r->src.x + op->src.offset[0],
						r->src.y + op->src.offset[1] + r->height,
						op->src.transform,
						&src_x[1],
						&src_y[1]);

		sna_get_transformed_coordinates(r->src.x + op->src.offset[0] + r->width,
						r->src.y + op->src.offset[1] + r->height,
						op->src.transform,
						&src_x[2],
						&src_y[2]);
	} else {
		if (!sna_get_transformed_coordinates_3d(r->src.x + op->src.offset[0],
							r->src.y + op->src.offset[1],
							op->src.transform,
							&src_x[0],
							&src_y[0],
							&src_w[0]))
			return;

		if (!sna_get_transformed_coordinates_3d(r->src.x + op->src.offset[0],
							r->src.y + op->src.offset[1] + r->height,
							op->src.transform,
							&src_x[1],
							&src_y[1],
							&src_w[1]))
			return;

		if (!sna_get_transformed_coordinates_3d(r->src.x + op->src.offset[0] + r->width,
							r->src.y + op->src.offset[1] + r->height,
							op->src.transform,
							&src_x[2],
							&src_y[2],
							&src_w[2]))
			return;
	}

	if (op->mask.bo) {
		if (is_affine) {
			sna_get_transformed_coordinates(r->mask.x + op->mask.offset[0],
							r->mask.y + op->mask.offset[1],
							op->mask.transform,
							&mask_x[0],
							&mask_y[0]);

			sna_get_transformed_coordinates(r->mask.x + op->mask.offset[0],
							r->mask.y + op->mask.offset[1] + r->height,
							op->mask.transform,
							&mask_x[1],
							&mask_y[1]);

			sna_get_transformed_coordinates(r->mask.x + op->mask.offset[0] + r->width,
							r->mask.y + op->mask.offset[1] + r->height,
							op->mask.transform,
							&mask_x[2],
							&mask_y[2]);
		} else {
			if (!sna_get_transformed_coordinates_3d(r->mask.x + op->mask.offset[0],
								r->mask.y + op->mask.offset[1],
								op->mask.transform,
								&mask_x[0],
								&mask_y[0],
								&mask_w[0]))
				return;

			if (!sna_get_transformed_coordinates_3d(r->mask.x + op->mask.offset[0],
								r->mask.y + op->mask.offset[1] + r->height,
								op->mask.transform,
								&mask_x[1],
								&mask_y[1],
								&mask_w[1]))
				return;

			if (!sna_get_transformed_coordinates_3d(r->mask.x + op->mask.offset[0] + r->width,
								r->mask.y + op->mask.offset[1] + r->height,
								op->mask.transform,
								&mask_x[2],
								&mask_y[2],
								&mask_w[2]))
				return;
		}
	}

	OUT_VERTEX(r->dst.x + r->width, r->dst.y + r->height);
	OUT_VERTEX_F(src_x[2] * src_sf[0]);
	OUT_VERTEX_F(src_y[2] * src_sf[1]);
	if (!is_affine)
		OUT_VERTEX_F(src_w[2]);
	if (op->mask.bo) {
		OUT_VERTEX_F(mask_x[2] * mask_sf[0]);
		OUT_VERTEX_F(mask_y[2] * mask_sf[1]);
		if (!is_affine)
			OUT_VERTEX_F(mask_w[2]);
	}

	OUT_VERTEX(r->dst.x, r->dst.y + r->height);
	OUT_VERTEX_F(src_x[1] * src_sf[0]);
	OUT_VERTEX_F(src_y[1] * src_sf[1]);
	if (!is_affine)
		OUT_VERTEX_F(src_w[1]);
	if (op->mask.bo) {
		OUT_VERTEX_F(mask_x[1] * mask_sf[0]);
		OUT_VERTEX_F(mask_y[1] * mask_sf[1]);
		if (!is_affine)
			OUT_VERTEX_F(mask_w[1]);
	}

	OUT_VERTEX(r->dst.x, r->dst.y);
	OUT_VERTEX_F(src_x[0] * src_sf[0]);
	OUT_VERTEX_F(src_y[0] * src_sf[1]);
	if (!is_affine)
		OUT_VERTEX_F(src_w[0]);
	if (op->mask.bo) {
		OUT_VERTEX_F(mask_x[0] * mask_sf[0]);
		OUT_VERTEX_F(mask_y[0] * mask_sf[1]);
		if (!is_affine)
			OUT_VERTEX_F(mask_w[0]);
	}
}

static void gen7_emit_vertex_buffer(struct sna *sna,
				    const struct sna_composite_op *op)
{
	int id = op->u.gen7.ve_id;

	OUT_BATCH(GEN7_3DSTATE_VERTEX_BUFFERS | (5 - 2));
	OUT_BATCH(id << GEN7_VB0_BUFFER_INDEX_SHIFT |
		  GEN7_VB0_VERTEXDATA |
		  GEN7_VB0_ADDRESS_MODIFY_ENABLE |
		  4*op->floats_per_vertex << GEN7_VB0_BUFFER_PITCH_SHIFT);
	sna->render.vertex_reloc[id] = sna->kgem.nbatch;
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	sna->render_state.gen7.vb_id |= 1 << id;
}

static void gen7_emit_primitive(struct sna *sna)
{
	if (sna->kgem.nbatch == sna->render_state.gen7.last_primitive) {
		sna->render_state.gen7.vertex_offset = sna->kgem.nbatch - 5;
		return;
	}

	OUT_BATCH(GEN7_3DPRIMITIVE |
		  GEN7_3DPRIMITIVE_VERTEX_SEQUENTIAL |
		  _3DPRIM_RECTLIST << GEN7_3DPRIMITIVE_TOPOLOGY_SHIFT |
		  0 << 9 |
		  4);
	sna->render_state.gen7.vertex_offset = sna->kgem.nbatch;
	OUT_BATCH(0);	/* vertex count, to be filled in later */
	OUT_BATCH(sna->render.vertex_index);
	OUT_BATCH(1);	/* single instance */
	OUT_BATCH(0);	/* start instance location */
	OUT_BATCH(0);	/* index buffer offset, ignored */
	sna->render.vertex_start = sna->render.vertex_index;

	sna->render_state.gen7.last_primitive = sna->kgem.nbatch;
}

static bool gen7_rectangle_begin(struct sna *sna,
				 const struct sna_composite_op *op)
{
	int id = op->u.gen7.ve_id;
	int ndwords;

	ndwords = 0;
	if ((sna->render_state.gen7.vb_id & (1 << id)) == 0)
		ndwords += 5;
	if (sna->render_state.gen7.vertex_offset == 0)
		ndwords += op->need_magic_ca_pass ? 60 : 6;
	if (ndwords == 0)
		return true;

	if (!kgem_check_batch(&sna->kgem, ndwords))
		return false;

	if ((sna->render_state.gen7.vb_id & (1 << id)) == 0)
		gen7_emit_vertex_buffer(sna, op);
	if (sna->render_state.gen7.vertex_offset == 0)
		gen7_emit_primitive(sna);

	return true;
}

static int gen7_get_rectangles__flush(struct sna *sna, bool ca)
{
	if (!kgem_check_batch(&sna->kgem, ca ? 65 : 5))
		return 0;
	if (sna->kgem.nexec > KGEM_EXEC_SIZE(&sna->kgem) - 1)
		return 0;
	if (sna->kgem.nreloc > KGEM_RELOC_SIZE(&sna->kgem) - 1)
		return 0;

	gen7_vertex_finish(sna, FALSE);
	sna->render.vertex_index = 0;

	return  ARRAY_SIZE(sna->render.vertex_data);
}

inline static int gen7_get_rectangles(struct sna *sna,
				      const struct sna_composite_op *op,
				      int want)
{
	int rem = vertex_space(sna);

	if (rem < 3*op->floats_per_vertex) {
		DBG(("flushing vbo for %s: %d < %d\n",
		     __FUNCTION__, rem, 3*op->floats_per_vertex));
		rem = gen7_get_rectangles__flush(sna, op->need_magic_ca_pass);
		if (rem == 0)
			return 0;
	}

	if (!gen7_rectangle_begin(sna, op))
		return 0;

	if (want > 1 && want * op->floats_per_vertex*3 > rem)
		want = rem / (3*op->floats_per_vertex);

	sna->render.vertex_index += 3*want;
	return want;
}

inline static uint32_t *gen7_composite_get_binding_table(struct sna *sna,
							 const struct sna_composite_op *op,
							 uint16_t *offset)
{
	uint32_t *table;

	sna->kgem.surface -=
		sizeof(struct gen7_surface_state_padded) / sizeof(uint32_t);
	/* Clear all surplus entries to zero in case of prefetch */
	table = memset(sna->kgem.batch + sna->kgem.surface,
		       0, sizeof(struct gen7_surface_state_padded));

	DBG(("%s(%x)\n", __FUNCTION__, 4*sna->kgem.surface));

	*offset = sna->kgem.surface;
	return table;
}

static uint32_t
gen7_choose_composite_vertex_buffer(struct sna *sna,
				    const struct sna_composite_op *op)
{
	int has_mask = op->mask.bo != NULL;
	int is_affine = op->is_affine;
	return has_mask << 1 | is_affine;
}

static void
gen7_get_batch(struct sna *sna)
{
	kgem_set_mode(&sna->kgem, KGEM_RENDER);

	if (!kgem_check_batch_with_surfaces(&sna->kgem, 150, 4)) {
		DBG(("%s: flushing batch: %d < %d+%d\n",
		     __FUNCTION__, sna->kgem.surface - sna->kgem.nbatch,
		     150, 4*8));
		kgem_submit(&sna->kgem);
	}

	if (sna->render_state.gen7.needs_invariant)
		gen7_emit_invariant(sna);
}

static void gen7_emit_composite_state(struct sna *sna,
				      const struct sna_composite_op *op)
{
	uint32_t *binding_table;
	uint16_t offset;

	gen7_get_batch(sna);

	binding_table = gen7_composite_get_binding_table(sna, op, &offset);

	binding_table[0] =
		gen7_bind_bo(sna,
			    op->dst.bo, op->dst.width, op->dst.height,
			    gen7_get_dest_format(op->dst.format),
			    TRUE);
	binding_table[1] =
		gen7_bind_bo(sna,
			     op->src.bo, op->src.width, op->src.height,
			     op->src.card_format,
			     FALSE);
	if (op->mask.bo) {
		binding_table[2] =
			gen7_bind_bo(sna,
				     op->mask.bo,
				     op->mask.width,
				     op->mask.height,
				     op->mask.card_format,
				     FALSE);
	}

	if (sna->kgem.surface == offset&&
	    *(uint64_t *)(sna->kgem.batch + sna->render_state.gen7.surface_table) == *(uint64_t*)binding_table &&
	    (op->mask.bo == NULL ||
	     sna->kgem.batch[sna->render_state.gen7.surface_table+2] == binding_table[2])) {
		sna->kgem.surface += sizeof(struct gen7_surface_state_padded) / sizeof(uint32_t);
		offset = sna->render_state.gen7.surface_table;
	}

	gen7_emit_state(sna, op, offset);
}

static void
gen7_align_vertex(struct sna *sna, const struct sna_composite_op *op)
{
	if (op->floats_per_vertex != sna->render_state.gen7.floats_per_vertex) {
		DBG(("aligning vertex: was %d, now %d floats per vertex, %d->%d\n",
		     sna->render_state.gen7.floats_per_vertex,
		     op->floats_per_vertex,
		     sna->render.vertex_index,
		     (sna->render.vertex_used + op->floats_per_vertex - 1) / op->floats_per_vertex));
		sna->render.vertex_index = (sna->render.vertex_used + op->floats_per_vertex - 1) / op->floats_per_vertex;
		sna->render.vertex_used = sna->render.vertex_index * op->floats_per_vertex;
		sna->render_state.gen7.floats_per_vertex = op->floats_per_vertex;
	}
}

fastcall static void
gen7_render_composite_blt(struct sna *sna,
			  const struct sna_composite_op *op,
			  const struct sna_composite_rectangles *r)
{
	if (!gen7_get_rectangles(sna, op, 1)) {
		gen7_emit_composite_state(sna, op);
		gen7_get_rectangles(sna, op, 1);
	}

	op->prim_emit(sna, op, r);
}

static void
gen7_render_composite_boxes(struct sna *sna,
			    const struct sna_composite_op *op,
			    const BoxRec *box, int nbox)
{
	DBG(("composite_boxes(%d)\n", nbox));

	do {
		int nbox_this_time = gen7_get_rectangles(sna, op, nbox);
		if (nbox_this_time == 0) {
			gen7_emit_composite_state(sna, op);
			nbox_this_time = gen7_get_rectangles(sna, op, nbox);
		}
		nbox -= nbox_this_time;
		do {
			struct sna_composite_rectangles r;

			DBG(("  %s: (%d, %d), (%d, %d)\n",
			     __FUNCTION__,
			     box->x1, box->y1, box->x2, box->y2));

			r.dst.x = box->x1;
			r.dst.y = box->y1;
			r.width  = box->x2 - box->x1;
			r.height = box->y2 - box->y1;
			r.src = r.mask = r.dst;

			op->prim_emit(sna, op, &r);
			box++;
		} while (--nbox_this_time);
	} while (nbox);
}

#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

static uint32_t
gen7_composite_create_blend_state(struct sna_static_stream *stream)
{
	char *base, *ptr;
	int src, dst;

	base = sna_static_stream_map(stream,
				     GEN7_BLENDFACTOR_COUNT * GEN7_BLENDFACTOR_COUNT * GEN7_BLEND_STATE_PADDED_SIZE,
				     64);

	ptr = base;
	for (src = 0; src < GEN7_BLENDFACTOR_COUNT; src++) {
		for (dst= 0; dst < GEN7_BLENDFACTOR_COUNT; dst++) {
			struct gen7_blend_state *blend =
				(struct gen7_blend_state *)ptr;

			blend->blend0.dest_blend_factor = dst;
			blend->blend0.source_blend_factor = src;
			blend->blend0.blend_func = GEN7_BLENDFUNCTION_ADD;
			blend->blend0.blend_enable = 1;

			blend->blend1.post_blend_clamp_enable = 1;
			blend->blend1.pre_blend_clamp_enable = 1;

			ptr += GEN7_BLEND_STATE_PADDED_SIZE;
		}
	}

	return sna_static_stream_offsetof(stream, base);
}

static uint32_t gen7_bind_video_source(struct sna *sna,
				       struct kgem_bo *src_bo,
				       uint32_t src_offset,
				       int src_width,
				       int src_height,
				       int src_pitch,
				       uint32_t src_surf_format)
{
	struct gen7_surface_state *ss;

	sna->kgem.surface -= sizeof(struct gen7_surface_state_padded) / sizeof(uint32_t);

	ss = memset(sna->kgem.batch + sna->kgem.surface, 0, sizeof(*ss));
	ss->ss0.surface_type = GEN7_SURFACE_2D;
	ss->ss0.surface_format = src_surf_format;

	ss->ss1.base_addr =
		kgem_add_reloc(&sna->kgem,
			       sna->kgem.surface + 1,
			       src_bo,
			       I915_GEM_DOMAIN_SAMPLER << 16,
			       src_offset);

	ss->ss2.width  = src_width - 1;
	ss->ss2.height = src_height - 1;
	ss->ss3.pitch  = src_pitch - 1;

	return sna->kgem.surface * sizeof(uint32_t);
}

static void gen7_emit_video_state(struct sna *sna,
				  struct sna_composite_op *op,
				  struct sna_video_frame *frame)
{
	uint32_t src_surf_format;
	uint32_t src_surf_base[6];
	int src_width[6];
	int src_height[6];
	int src_pitch[6];
	uint32_t *binding_table;
	uint16_t offset;
	int n_src, n;

	gen7_get_batch(sna);

	src_surf_base[0] = 0;
	src_surf_base[1] = 0;
	src_surf_base[2] = frame->VBufOffset;
	src_surf_base[3] = frame->VBufOffset;
	src_surf_base[4] = frame->UBufOffset;
	src_surf_base[5] = frame->UBufOffset;

	if (is_planar_fourcc(frame->id)) {
		src_surf_format = GEN7_SURFACEFORMAT_R8_UNORM;
		src_width[1]  = src_width[0]  = frame->width;
		src_height[1] = src_height[0] = frame->height;
		src_pitch[1]  = src_pitch[0]  = frame->pitch[1];
		src_width[4]  = src_width[5]  = src_width[2]  = src_width[3] =
			frame->width / 2;
		src_height[4] = src_height[5] = src_height[2] = src_height[3] =
			frame->height / 2;
		src_pitch[4]  = src_pitch[5]  = src_pitch[2]  = src_pitch[3] =
			frame->pitch[0];
		n_src = 6;
	} else {
		if (frame->id == FOURCC_UYVY)
			src_surf_format = GEN7_SURFACEFORMAT_YCRCB_SWAPY;
		else
			src_surf_format = GEN7_SURFACEFORMAT_YCRCB_NORMAL;

		src_width[0]  = frame->width;
		src_height[0] = frame->height;
		src_pitch[0]  = frame->pitch[0];
		n_src = 1;
	}

	binding_table = gen7_composite_get_binding_table(sna, op, &offset);

	binding_table[0] =
		gen7_bind_bo(sna,
			     op->dst.bo, op->dst.width, op->dst.height,
			     gen7_get_dest_format(op->dst.format),
			     TRUE);
	for (n = 0; n < n_src; n++) {
		binding_table[1+n] =
			gen7_bind_video_source(sna,
					       frame->bo,
					       src_surf_base[n],
					       src_width[n],
					       src_height[n],
					       src_pitch[n],
					       src_surf_format);
	}

	gen7_emit_state(sna, op, offset);
}

static Bool
gen7_render_video(struct sna *sna,
		  struct sna_video *video,
		  struct sna_video_frame *frame,
		  RegionPtr dstRegion,
		  short src_w, short src_h,
		  short drw_w, short drw_h,
		  PixmapPtr pixmap)
{
	struct sna_composite_op tmp;
	int nbox, dxo, dyo, pix_xoff, pix_yoff;
	float src_scale_x, src_scale_y;
	struct sna_pixmap *priv;
	BoxPtr box;

	DBG(("%s: src=(%d, %d), dst=(%d, %d), %dx[(%d, %d), (%d, %d)...]\n",
	     __FUNCTION__, src_w, src_h, drw_w, drw_h,
	     REGION_NUM_RECTS(dstRegion),
	     REGION_EXTENTS(NULL, dstRegion)->x1,
	     REGION_EXTENTS(NULL, dstRegion)->y1,
	     REGION_EXTENTS(NULL, dstRegion)->x2,
	     REGION_EXTENTS(NULL, dstRegion)->y2));

	priv = sna_pixmap_force_to_gpu(pixmap);
	if (priv == NULL)
		return FALSE;

	memset(&tmp, 0, sizeof(tmp));

	tmp.op = PictOpSrc;
	tmp.dst.pixmap = pixmap;
	tmp.dst.width  = pixmap->drawable.width;
	tmp.dst.height = pixmap->drawable.height;
	tmp.dst.format = sna_format_for_depth(pixmap->drawable.depth);
	tmp.dst.bo = priv->gpu_bo;

	tmp.src.filter = SAMPLER_FILTER_BILINEAR;
	tmp.src.repeat = SAMPLER_EXTEND_PAD;

	tmp.is_affine = TRUE;
	tmp.floats_per_vertex = 3;

	if (is_planar_fourcc(frame->id)) {
		tmp.u.gen7.wm_kernel = GEN7_WM_KERNEL_VIDEO_PLANAR;
		tmp.u.gen7.nr_surfaces = 7;
	} else {
		tmp.u.gen7.wm_kernel = GEN7_WM_KERNEL_VIDEO_PACKED;
		tmp.u.gen7.nr_surfaces = 2;
	}
	tmp.u.gen7.nr_inputs = 1;
	tmp.u.gen7.ve_id = 1;

	if (!kgem_check_bo(&sna->kgem, tmp.dst.bo))
		kgem_submit(&sna->kgem);
	if (!kgem_check_bo(&sna->kgem, frame->bo))
		kgem_submit(&sna->kgem);

	if (kgem_bo_is_dirty(frame->bo))
		kgem_emit_flush(&sna->kgem);

	gen7_emit_video_state(sna, &tmp, frame);
	gen7_align_vertex(sna, &tmp);

	/* Set up the offset for translating from the given region (in screen
	 * coordinates) to the backing pixmap.
	 */
#ifdef COMPOSITE
	pix_xoff = -pixmap->screen_x + pixmap->drawable.x;
	pix_yoff = -pixmap->screen_y + pixmap->drawable.y;
#else
	pix_xoff = 0;
	pix_yoff = 0;
#endif

	dxo = dstRegion->extents.x1;
	dyo = dstRegion->extents.y1;

	/* Use normalized texture coordinates */
	src_scale_x = ((float)src_w / frame->width) / (float)drw_w;
	src_scale_y = ((float)src_h / frame->height) / (float)drw_h;

	box = REGION_RECTS(dstRegion);
	nbox = REGION_NUM_RECTS(dstRegion);
	while (nbox--) {
		BoxRec r;

		r.x1 = box->x1 + pix_xoff;
		r.x2 = box->x2 + pix_xoff;
		r.y1 = box->y1 + pix_yoff;
		r.y2 = box->y2 + pix_yoff;

		if (!gen7_get_rectangles(sna, &tmp, 1)) {
			gen7_emit_video_state(sna, &tmp, frame);
			gen7_get_rectangles(sna, &tmp, 1);
		}

		OUT_VERTEX(r.x2, r.y2);
		OUT_VERTEX_F((box->x2 - dxo) * src_scale_x);
		OUT_VERTEX_F((box->y2 - dyo) * src_scale_y);

		OUT_VERTEX(r.x1, r.y2);
		OUT_VERTEX_F((box->x1 - dxo) * src_scale_x);
		OUT_VERTEX_F((box->y2 - dyo) * src_scale_y);

		OUT_VERTEX(r.x1, r.y1);
		OUT_VERTEX_F((box->x1 - dxo) * src_scale_x);
		OUT_VERTEX_F((box->y1 - dyo) * src_scale_y);

		sna_damage_add_box(&priv->gpu_damage, &r);
		sna_damage_subtract_box(&priv->cpu_damage, &r);
		box++;
	}

	return TRUE;
}

static Bool
gen7_composite_solid_init(struct sna *sna,
			  struct sna_composite_channel *channel,
			  uint32_t color)
{
	DBG(("%s: color=%x\n", __FUNCTION__, color));

	channel->filter = PictFilterNearest;
	channel->repeat = RepeatNormal;
	channel->is_affine = TRUE;
	channel->is_solid  = TRUE;
	channel->transform = NULL;
	channel->width  = 1;
	channel->height = 1;
	channel->card_format = GEN7_SURFACEFORMAT_B8G8R8A8_UNORM;

	channel->bo = sna_render_get_solid(sna, color);

	channel->scale[0]  = channel->scale[1]  = 1;
	channel->offset[0] = channel->offset[1] = 0;
	return channel->bo != NULL;
}

static int
gen7_composite_picture(struct sna *sna,
		       PicturePtr picture,
		       struct sna_composite_channel *channel,
		       int x, int y,
		       int w, int h,
		       int dst_x, int dst_y)
{
	PixmapPtr pixmap;
	uint32_t color;
	int16_t dx, dy;

	DBG(("%s: (%d, %d)x(%d, %d), dst=(%d, %d)\n",
	     __FUNCTION__, x, y, w, h, dst_x, dst_y));

	channel->is_solid = FALSE;
	channel->card_format = -1;

	if (sna_picture_is_solid(picture, &color))
		return gen7_composite_solid_init(sna, channel, color);

	if (picture->pDrawable == NULL)
		return sna_render_picture_fixup(sna, picture, channel,
						x, y, w, h, dst_x, dst_y);

	if (!gen7_check_repeat(picture))
		return sna_render_picture_fixup(sna, picture, channel,
						x, y, w, h, dst_x, dst_y);

	if (!gen7_check_filter(picture))
		return sna_render_picture_fixup(sna, picture, channel,
						x, y, w, h, dst_x, dst_y);

	channel->repeat = picture->repeat ? picture->repeatType : RepeatNone;
	channel->filter = picture->filter;

	pixmap = get_drawable_pixmap(picture->pDrawable);
	get_drawable_deltas(picture->pDrawable, pixmap, &dx, &dy);

	x += dx + picture->pDrawable->x;
	y += dy + picture->pDrawable->y;

	channel->is_affine = sna_transform_is_affine(picture->transform);
	if (sna_transform_is_integer_translation(picture->transform, &dx, &dy)) {
		DBG(("%s: integer translation (%d, %d), removing\n",
		     __FUNCTION__, dx, dy));
		x += dx;
		y += dy;
		channel->transform = NULL;
		channel->filter = PictFilterNearest;
	} else
		channel->transform = picture->transform;

	channel->card_format = gen7_get_card_format(picture->format);
	if (channel->card_format == -1)
		return sna_render_picture_convert(sna, picture, channel, pixmap,
						  x, y, w, h, dst_x, dst_y);

	if (pixmap->drawable.width > GEN7_MAX_SIZE ||
	    pixmap->drawable.height > GEN7_MAX_SIZE) {
		DBG(("%s: extracting from pixmap %dx%d\n", __FUNCTION__,
		     pixmap->drawable.width, pixmap->drawable.height));
		return sna_render_picture_extract(sna, picture, channel,
						  x, y, w, h, dst_x, dst_y);
	}

	return sna_render_pixmap_bo(sna, channel, pixmap,
				    x, y, w, h, dst_x, dst_y);
}

static void gen7_composite_channel_convert(struct sna_composite_channel *channel)
{
	channel->repeat = gen7_repeat(channel->repeat);
	channel->filter = gen7_filter(channel->filter);
	if (channel->card_format == -1)
		channel->card_format = gen7_get_card_format(channel->pict_format);
	assert(channel->card_format != -1);
}

static void gen7_render_composite_done(struct sna *sna,
				       const struct sna_composite_op *op)
{
	gen7_vertex_flush(sna);
	_kgem_set_mode(&sna->kgem, KGEM_RENDER);
	sna->render.op = NULL;

	sna_render_composite_redirect_done(sna, op);

	if (op->src.bo)
		kgem_bo_destroy(&sna->kgem, op->src.bo);
	if (op->mask.bo)
		kgem_bo_destroy(&sna->kgem, op->mask.bo);
}

static Bool
gen7_composite_set_target(struct sna *sna,
			  struct sna_composite_op *op,
			  PicturePtr dst)
{
	struct sna_pixmap *priv;

	if (!gen7_check_dst_format(dst->format)) {
		DBG(("%s: unsupported target format %08x\n",
		     __FUNCTION__, dst->format));
		return FALSE;
	}

	op->dst.pixmap = get_drawable_pixmap(dst->pDrawable);
	op->dst.width  = op->dst.pixmap->drawable.width;
	op->dst.height = op->dst.pixmap->drawable.height;
	op->dst.format = dst->format;
	priv = sna_pixmap(op->dst.pixmap);

	op->dst.bo = NULL;
	if (priv && priv->gpu_bo == NULL) {
		op->dst.bo = priv->cpu_bo;
		op->damage = &priv->cpu_damage;
	}
	if (op->dst.bo == NULL) {
		priv = sna_pixmap_force_to_gpu(op->dst.pixmap);
		if (priv == NULL)
			return FALSE;

		op->dst.bo = priv->gpu_bo;
		if (!priv->gpu_only &&
		    !sna_damage_is_all(&priv->gpu_damage, op->dst.width, op->dst.height))
			op->damage = &priv->gpu_damage;
	}

	get_drawable_deltas(dst->pDrawable, op->dst.pixmap,
			    &op->dst.x, &op->dst.y);

	DBG(("%s: pixmap=%p, format=%08x, size=%dx%d, pitch=%d, delta=(%d,%d)\n",
	     __FUNCTION__,
	     op->dst.pixmap, (int)op->dst.format,
	     op->dst.width, op->dst.height,
	     op->dst.bo->pitch,
	     op->dst.x, op->dst.y));
	return TRUE;
}

static Bool
try_blt(struct sna *sna, PicturePtr dst, int width, int height)
{
	if (sna->kgem.mode == KGEM_BLT) {
		DBG(("%s: already performing BLT\n", __FUNCTION__));
		return TRUE;
	}

	if (width > GEN7_MAX_SIZE || height > GEN7_MAX_SIZE) {
		DBG(("%s: operation too large for 3D pipe (%d, %d)\n",
		     __FUNCTION__, width, height));
		return TRUE;
	}

	return FALSE;
}

static Bool
gen7_render_composite(struct sna *sna,
		      uint8_t op,
		      PicturePtr src,
		      PicturePtr mask,
		      PicturePtr dst,
		      int16_t src_x, int16_t src_y,
		      int16_t msk_x, int16_t msk_y,
		      int16_t dst_x, int16_t dst_y,
		      int16_t width, int16_t height,
		      struct sna_composite_op *tmp)
{

#if NO_COMPOSITE
	if (mask)
		return FALSE;

	return sna_blt_composite(sna, op,
				 src, dst,
				 src_x, src_y,
				 dst_x, dst_y,
				 width, height, tmp);
#endif

	DBG(("%s: %dx%d, current mode=%d\n", __FUNCTION__,
	     width, height, sna->kgem.mode));

	if (mask == NULL &&
	    try_blt(sna, dst, width, height) &&
	    sna_blt_composite(sna, op,
			      src, dst,
			      src_x, src_y,
			      dst_x, dst_y,
			      width, height, tmp))
		return TRUE;

	if (op >= ARRAY_SIZE(gen7_blend_op))
		return FALSE;

	if (need_tiling(sna, width, height))
		return sna_tiling_composite(sna,
					    op, src, mask, dst,
					    src_x, src_y,
					    msk_x, msk_y,
					    dst_x, dst_y,
					    width, height,
					    tmp);

	tmp->op = op;
	if (!gen7_composite_set_target(sna, tmp, dst))
		return FALSE;

	if (tmp->dst.width > GEN7_MAX_SIZE || tmp->dst.height > GEN7_MAX_SIZE) {
		if (!sna_render_composite_redirect(sna, tmp,
						   dst_x, dst_y, width, height))
			return FALSE;
	}

	switch (gen7_composite_picture(sna, src, &tmp->src,
				       src_x, src_y,
				       width, height,
				       dst_x, dst_y)) {
	case -1:
		goto cleanup_dst;
	case 0:
		gen7_composite_solid_init(sna, &tmp->src, 0);
	case 1:
		gen7_composite_channel_convert(&tmp->src);
		break;
	}

	tmp->is_affine = tmp->src.is_affine;
	tmp->has_component_alpha = FALSE;
	tmp->need_magic_ca_pass = FALSE;

	tmp->mask.bo = NULL;
	tmp->mask.filter = SAMPLER_FILTER_NEAREST;
	tmp->mask.repeat = SAMPLER_EXTEND_NONE;

	tmp->prim_emit = gen7_emit_composite_primitive;
	if (mask) {
		if (mask->componentAlpha && PICT_FORMAT_RGB(mask->format)) {
			tmp->has_component_alpha = TRUE;

			/* Check if it's component alpha that relies on a source alpha and on
			 * the source value.  We can only get one of those into the single
			 * source value that we get to blend with.
			 */
			if (gen7_blend_op[op].src_alpha &&
			    (gen7_blend_op[op].src_blend != GEN7_BLENDFACTOR_ZERO)) {
				if (op != PictOpOver)
					goto cleanup_src;

				tmp->need_magic_ca_pass = TRUE;
				tmp->op = PictOpOutReverse;
			}
		}

		switch (gen7_composite_picture(sna, mask, &tmp->mask,
					       msk_x, msk_y,
					       width, height,
					       dst_x, dst_y)) {
		case -1:
			goto cleanup_src;
		case 0:
			gen7_composite_solid_init(sna, &tmp->mask, 0);
		case 1:
			gen7_composite_channel_convert(&tmp->mask);
			break;
		}

		tmp->is_affine &= tmp->mask.is_affine;

		if (tmp->src.transform == NULL && tmp->mask.transform == NULL)
			tmp->prim_emit = gen7_emit_composite_primitive_identity_source_mask;

		tmp->floats_per_vertex = 5 + 2 * !tmp->is_affine;
	} else {
		if (tmp->src.is_solid)
			tmp->prim_emit = gen7_emit_composite_primitive_solid;
		else if (tmp->src.transform == NULL)
			tmp->prim_emit = gen7_emit_composite_primitive_identity_source;
		else if (tmp->src.is_affine)
			tmp->prim_emit = gen7_emit_composite_primitive_affine_source;

		tmp->floats_per_vertex = 3 + !tmp->is_affine;
	}

	tmp->u.gen7.wm_kernel =
		gen7_choose_composite_kernel(tmp->op,
					     tmp->mask.bo != NULL,
					     tmp->has_component_alpha,
					     tmp->is_affine);
	tmp->u.gen7.nr_surfaces = 2 + (tmp->mask.bo != NULL);
	tmp->u.gen7.nr_inputs = 1 + (tmp->mask.bo != NULL);
	tmp->u.gen7.ve_id =
		gen7_choose_composite_vertex_buffer(sna, tmp);

	tmp->blt   = gen7_render_composite_blt;
	tmp->boxes = gen7_render_composite_boxes;
	tmp->done  = gen7_render_composite_done;

	if (!kgem_check_bo(&sna->kgem, tmp->dst.bo))
		kgem_submit(&sna->kgem);
	if (!kgem_check_bo(&sna->kgem, tmp->src.bo))
		kgem_submit(&sna->kgem);
	if (!kgem_check_bo(&sna->kgem, tmp->mask.bo))
		kgem_submit(&sna->kgem);

	if (kgem_bo_is_dirty(tmp->src.bo) || kgem_bo_is_dirty(tmp->mask.bo))
		kgem_emit_flush(&sna->kgem);

	gen7_emit_composite_state(sna, tmp);
	gen7_align_vertex(sna, tmp);

	sna->render.op = tmp;
	return TRUE;

cleanup_src:
	if (tmp->src.bo)
		kgem_bo_destroy(&sna->kgem, tmp->src.bo);
cleanup_dst:
	if (tmp->redirect.real_bo)
		kgem_bo_destroy(&sna->kgem, tmp->dst.bo);
	return FALSE;
}

static void
gen7_emit_copy_state(struct sna *sna,
		     const struct sna_composite_op *op)
{
	uint32_t *binding_table;
	uint16_t offset;

	gen7_get_batch(sna);

	binding_table = gen7_composite_get_binding_table(sna, op, &offset);

	binding_table[0] =
		gen7_bind_bo(sna,
			     op->dst.bo, op->dst.width, op->dst.height,
			     gen7_get_dest_format_for_depth(op->dst.pixmap->drawable.depth),
			     TRUE);
	binding_table[1] =
		gen7_bind_bo(sna,
			     op->src.bo, op->src.width, op->src.height,
			     op->src.card_format,
			     FALSE);

	if (sna->kgem.surface == offset &&
	    *(uint64_t *)(sna->kgem.batch + sna->render_state.gen7.surface_table) == *(uint64_t*)binding_table) {
		sna->kgem.surface += sizeof(struct gen7_surface_state_padded) / sizeof(uint32_t);
		offset = sna->render_state.gen7.surface_table;
	}

	gen7_emit_state(sna, op, offset);
}

static Bool
gen7_render_copy_boxes(struct sna *sna, uint8_t alu,
		       PixmapPtr src, struct kgem_bo *src_bo, int16_t src_dx, int16_t src_dy,
		       PixmapPtr dst, struct kgem_bo *dst_bo, int16_t dst_dx, int16_t dst_dy,
		       const BoxRec *box, int n)
{
	struct sna_composite_op tmp;

#if NO_COPY_BOXES
	if (!sna_blt_compare_depth(&src->drawable, &dst->drawable))
		return FALSE;

	return sna_blt_copy_boxes(sna, alu,
				  src_bo, src_dx, src_dy,
				  dst_bo, dst_dx, dst_dy,
				  dst->drawable.bitsPerPixel,
				  box, n);
#endif

	DBG(("%s (%d, %d)->(%d, %d) x %d, alu=%x, self-copy=%d\n",
	     __FUNCTION__, src_dx, src_dy, dst_dx, dst_dy, n, alu,
	     src_bo == dst_bo));

	if (sna->kgem.mode == KGEM_BLT &&
	    sna_blt_compare_depth(&src->drawable, &dst->drawable) &&
	    sna_blt_copy_boxes(sna, alu,
			       src_bo, src_dx, src_dy,
			       dst_bo, dst_dx, dst_dy,
			       dst->drawable.bitsPerPixel,
			       box, n))
		return TRUE;

	if (!(alu == GXcopy || alu == GXclear) || src_bo == dst_bo ||
	    src->drawable.width > GEN7_MAX_SIZE ||
	    src->drawable.height > GEN7_MAX_SIZE ||
	    dst->drawable.width > GEN7_MAX_SIZE ||
	    dst->drawable.height > GEN7_MAX_SIZE) {
		if (!sna_blt_compare_depth(&src->drawable, &dst->drawable))
			return FALSE;

		return sna_blt_copy_boxes(sna, alu,
					  src_bo, src_dx, src_dy,
					  dst_bo, dst_dx, dst_dy,
					  dst->drawable.bitsPerPixel,
					  box, n);
	}
	tmp.op = alu == GXcopy ? PictOpSrc : PictOpClear;

	tmp.dst.pixmap = dst;
	tmp.dst.x = tmp.dst.y = 0;
	tmp.dst.width  = dst->drawable.width;
	tmp.dst.height = dst->drawable.height;
	tmp.dst.format = sna_format_for_depth(dst->drawable.depth);
	tmp.dst.bo = dst_bo;
	tmp.dst.x = dst_dx;
	tmp.dst.y = dst_dy;

	tmp.src.bo = src_bo;
	tmp.src.filter = SAMPLER_FILTER_NEAREST;
	tmp.src.repeat = SAMPLER_EXTEND_NONE;
	tmp.src.card_format =
		gen7_get_card_format_for_depth(src->drawable.depth),
	tmp.src.width  = src->drawable.width;
	tmp.src.height = src->drawable.height;

	tmp.mask.bo = NULL;
	tmp.mask.filter = SAMPLER_FILTER_NEAREST;
	tmp.mask.repeat = SAMPLER_EXTEND_NONE;

	tmp.is_affine = TRUE;
	tmp.floats_per_vertex = 3;
	tmp.has_component_alpha = 0;
	tmp.need_magic_ca_pass = 0;

	tmp.u.gen7.wm_kernel = GEN7_WM_KERNEL_NOMASK;
	tmp.u.gen7.nr_surfaces = 2;
	tmp.u.gen7.nr_inputs = 1;
	tmp.u.gen7.ve_id = 1;

	if (!kgem_check_bo(&sna->kgem, dst_bo))
		kgem_submit(&sna->kgem);
	if (!kgem_check_bo(&sna->kgem, src_bo))
		kgem_submit(&sna->kgem);

	if (kgem_bo_is_dirty(src_bo))
		kgem_emit_flush(&sna->kgem);

	gen7_emit_copy_state(sna, &tmp);
	gen7_align_vertex(sna, &tmp);

	tmp.src.scale[0] = 1.f / src->drawable.width;
	tmp.src.scale[1] = 1.f / src->drawable.height;
	do {
		float *v;
		int n_this_time = gen7_get_rectangles(sna, &tmp, n);
		if (n_this_time == 0) {
			gen7_emit_copy_state(sna, &tmp);
			n_this_time = gen7_get_rectangles(sna, &tmp, n);
		}
		n -= n_this_time;

		v = sna->render.vertex_data + sna->render.vertex_used;
		sna->render.vertex_used += 9 * n_this_time;
		do {

			DBG(("	(%d, %d) -> (%d, %d) + (%d, %d)\n",
			     box->x1 + src_dx, box->y1 + src_dy,
			     box->x1 + dst_dx, box->y1 + dst_dy,
			     box->x2 - box->x1, box->y2 - box->y1));
			v[0] = pack_2s(box->x2, box->y2);
			v[3] = pack_2s(box->x1, box->y2);
			v[6] = pack_2s(box->x1, box->y1);

			v[1] = (box->x2 + src_dx) * tmp.src.scale[0];
			v[7] = v[4] = (box->x1 + src_dx) * tmp.src.scale[0];

			v[5] = v[2] = (box->y2 + src_dy) * tmp.src.scale[1];
			v[8] = (box->y1 + src_dy) * tmp.src.scale[1];

			v += 9;
			box++;
		} while (--n_this_time);
	} while (n);

	gen7_vertex_flush(sna);
	_kgem_set_mode(&sna->kgem, KGEM_RENDER);
	return TRUE;
}

static void
gen7_render_copy_blt(struct sna *sna,
		     const struct sna_copy_op *op,
		     int16_t sx, int16_t sy,
		     int16_t w,  int16_t h,
		     int16_t dx, int16_t dy)
{
	if (!gen7_get_rectangles(sna, &op->base, 1)) {
		gen7_emit_copy_state(sna, &op->base);
		gen7_get_rectangles(sna, &op->base, 1);
	}

	OUT_VERTEX(dx+w, dy+h);
	OUT_VERTEX_F((sx+w)*op->base.src.scale[0]);
	OUT_VERTEX_F((sy+h)*op->base.src.scale[1]);

	OUT_VERTEX(dx, dy+h);
	OUT_VERTEX_F(sx*op->base.src.scale[0]);
	OUT_VERTEX_F((sy+h)*op->base.src.scale[1]);

	OUT_VERTEX(dx, dy);
	OUT_VERTEX_F(sx*op->base.src.scale[0]);
	OUT_VERTEX_F(sy*op->base.src.scale[1]);
}

static void
gen7_render_copy_done(struct sna *sna, const struct sna_copy_op *op)
{
	gen7_vertex_flush(sna);
	_kgem_set_mode(&sna->kgem, KGEM_RENDER);
}

static Bool
gen7_render_copy(struct sna *sna, uint8_t alu,
		 PixmapPtr src, struct kgem_bo *src_bo,
		 PixmapPtr dst, struct kgem_bo *dst_bo,
		 struct sna_copy_op *op)
{
#if NO_COPY
	if (!sna_blt_compare_depth(&src->drawable, &dst->drawable))
		return FALSE;

	return sna_blt_copy(sna, alu,
			    src_bo, dst_bo,
			    dst->drawable.bitsPerPixel,
			    op);
#endif

	DBG(("%s (alu=%d, src=(%dx%d), dst=(%dx%d))\n",
	     __FUNCTION__, alu,
	     src->drawable.width, src->drawable.height,
	     dst->drawable.width, dst->drawable.height));

	if (sna->kgem.mode == KGEM_BLT &&
	    sna_blt_compare_depth(&src->drawable, &dst->drawable) &&
	    sna_blt_copy(sna, alu,
			 src_bo, dst_bo,
			 dst->drawable.bitsPerPixel,
			 op))
		return TRUE;

	if (!(alu == GXcopy || alu == GXclear) || src_bo == dst_bo ||
	    src->drawable.width > GEN7_MAX_SIZE ||
	    src->drawable.height > GEN7_MAX_SIZE ||
	    dst->drawable.width > GEN7_MAX_SIZE ||
	    dst->drawable.height > GEN7_MAX_SIZE) {
		if (!sna_blt_compare_depth(&src->drawable, &dst->drawable))
			return FALSE;

		return sna_blt_copy(sna, alu, src_bo, dst_bo,
				    dst->drawable.bitsPerPixel,
				    op);
	}

	op->base.op = alu == GXcopy ? PictOpSrc : PictOpClear;

	op->base.dst.pixmap = dst;
	op->base.dst.width  = dst->drawable.width;
	op->base.dst.height = dst->drawable.height;
	op->base.dst.format = sna_format_for_depth(dst->drawable.depth);
	op->base.dst.bo = dst_bo;

	op->base.src.bo = src_bo;
	op->base.src.card_format =
		gen7_get_card_format_for_depth(src->drawable.depth),
	op->base.src.width  = src->drawable.width;
	op->base.src.height = src->drawable.height;
	op->base.src.scale[0] = 1./src->drawable.width;
	op->base.src.scale[1] = 1./src->drawable.height;
	op->base.src.filter = SAMPLER_FILTER_NEAREST;
	op->base.src.repeat = SAMPLER_EXTEND_NONE;

	op->base.is_affine = true;
	op->base.floats_per_vertex = 3;

	op->base.u.gen7.wm_kernel = GEN7_WM_KERNEL_NOMASK;
	op->base.u.gen7.nr_surfaces = 2;
	op->base.u.gen7.nr_inputs = 1;
	op->base.u.gen7.ve_id = 1;

	if (!kgem_check_bo(&sna->kgem, dst_bo))
		kgem_submit(&sna->kgem);
	if (!kgem_check_bo(&sna->kgem, src_bo))
		kgem_submit(&sna->kgem);

	if (kgem_bo_is_dirty(src_bo))
		kgem_emit_flush(&sna->kgem);

	gen7_emit_copy_state(sna, &op->base);
	gen7_align_vertex(sna, &op->base);

	op->blt  = gen7_render_copy_blt;
	op->done = gen7_render_copy_done;
	return TRUE;
}

static void
gen7_emit_fill_state(struct sna *sna, const struct sna_composite_op *op)
{
	uint32_t *binding_table;
	uint16_t offset;

	/* XXX Render Target Fast Clear
	 * Set RTFC Enable in PS and render a rectangle.
	 * Limited to a clearing the full MSC surface only with a
	 * specific kernel.
	 */

	gen7_get_batch(sna);

	binding_table = gen7_composite_get_binding_table(sna, op, &offset);

	binding_table[0] =
		gen7_bind_bo(sna,
			     op->dst.bo, op->dst.width, op->dst.height,
			     gen7_get_dest_format(op->dst.format),
			     TRUE);
	binding_table[1] =
		gen7_bind_bo(sna,
			     op->src.bo, 1, 1,
			     GEN7_SURFACEFORMAT_B8G8R8A8_UNORM,
			     FALSE);

	if (sna->kgem.surface == offset &&
	    *(uint64_t *)(sna->kgem.batch + sna->render_state.gen7.surface_table) == *(uint64_t*)binding_table) {
		sna->kgem.surface +=
			sizeof(struct gen7_surface_state_padded)/sizeof(uint32_t);
		offset = sna->render_state.gen7.surface_table;
	}

	gen7_emit_state(sna, op, offset);
}

static Bool
gen7_render_fill_boxes(struct sna *sna,
		       CARD8 op,
		       PictFormat format,
		       const xRenderColor *color,
		       PixmapPtr dst, struct kgem_bo *dst_bo,
		       const BoxRec *box, int n)
{
	struct sna_composite_op tmp;
	uint32_t pixel;

	DBG(("%s (op=%d, color=(%04x, %04x, %04x, %04x) [%08x])\n",
	     __FUNCTION__, op,
	     color->red, color->green, color->blue, color->alpha, (int)format));

	if (op >= ARRAY_SIZE(gen7_blend_op)) {
		DBG(("%s: fallback due to unhandled blend op: %d\n",
		     __FUNCTION__, op));
		return FALSE;
	}

	if (sna->kgem.mode == KGEM_BLT ||
	    dst->drawable.width > GEN7_MAX_SIZE ||
	    dst->drawable.height > GEN7_MAX_SIZE ||
	    !gen7_check_dst_format(format)) {
		uint8_t alu = GXcopy;

		if (op == PictOpClear) {
			alu = GXclear;
			pixel = 0;
			op = PictOpSrc;
		}

		if (op == PictOpOver && color->alpha >= 0xff00)
			op = PictOpSrc;

		if (op == PictOpSrc &&
		    sna_get_pixel_from_rgba(&pixel,
					    color->red,
					    color->green,
					    color->blue,
					    color->alpha,
					    format) &&
		    sna_blt_fill_boxes(sna, alu,
				       dst_bo, dst->drawable.bitsPerPixel,
				       pixel, box, n))
			return TRUE;

		if (dst->drawable.width > GEN7_MAX_SIZE ||
		    dst->drawable.height > GEN7_MAX_SIZE ||
		    !gen7_check_dst_format(format))
			return FALSE;
	}

#if NO_FILL_BOXES
	return FALSE;
#endif

	if (!sna_get_pixel_from_rgba(&pixel,
				     color->red,
				     color->green,
				     color->blue,
				     color->alpha,
				     PICT_a8r8g8b8))
		return FALSE;

	DBG(("%s(%08x x %d [(%d, %d), (%d, %d) ...])\n",
	     __FUNCTION__, pixel, n,
	     box[0].x1, box[0].y1, box[0].x2, box[0].y2));

	memset(&tmp, 0, sizeof(tmp));

	tmp.op = op;

	tmp.dst.pixmap = dst;
	tmp.dst.width  = dst->drawable.width;
	tmp.dst.height = dst->drawable.height;
	tmp.dst.format = format;
	tmp.dst.bo = dst_bo;

	tmp.src.bo = sna_render_get_solid(sna, pixel);
	tmp.src.filter = SAMPLER_FILTER_NEAREST;
	tmp.src.repeat = SAMPLER_EXTEND_REPEAT;

	tmp.is_affine = TRUE;
	tmp.floats_per_vertex = 3;

	tmp.u.gen7.wm_kernel = GEN7_WM_KERNEL_NOMASK;
	tmp.u.gen7.nr_surfaces = 2;
	tmp.u.gen7.nr_inputs = 1;
	tmp.u.gen7.ve_id = 1;

	if (!kgem_check_bo(&sna->kgem, dst_bo))
		kgem_submit(&sna->kgem);

	gen7_emit_fill_state(sna, &tmp);
	gen7_align_vertex(sna, &tmp);

	do {
		int n_this_time = gen7_get_rectangles(sna, &tmp, n);
		if (n_this_time == 0) {
			gen7_emit_fill_state(sna, &tmp);
			n_this_time = gen7_get_rectangles(sna, &tmp, n);
		}
		n -= n_this_time;
		do {
			DBG(("	(%d, %d), (%d, %d)\n",
			     box->x1, box->y1, box->x2, box->y2));
			OUT_VERTEX(box->x2, box->y2);
			OUT_VERTEX_F(1);
			OUT_VERTEX_F(1);

			OUT_VERTEX(box->x1, box->y2);
			OUT_VERTEX_F(0);
			OUT_VERTEX_F(1);

			OUT_VERTEX(box->x1, box->y1);
			OUT_VERTEX_F(0);
			OUT_VERTEX_F(0);

			box++;
		} while (--n_this_time);
	} while (n);

	gen7_vertex_flush(sna);
	kgem_bo_destroy(&sna->kgem, tmp.src.bo);
	_kgem_set_mode(&sna->kgem, KGEM_RENDER);
	return TRUE;
}

static void
gen7_render_fill_blt(struct sna *sna,
		     const struct sna_fill_op *op,
		     int16_t x, int16_t y, int16_t w, int16_t h)
{
	DBG(("%s: (%d, %d)x(%d, %d)\n", __FUNCTION__, x, y, w, h));

	if (!gen7_get_rectangles(sna, &op->base, 1)) {
		gen7_emit_fill_state(sna, &op->base);
		gen7_get_rectangles(sna, &op->base, 1);
	}

	OUT_VERTEX(x+w, y+h);
	OUT_VERTEX_F(1);
	OUT_VERTEX_F(1);

	OUT_VERTEX(x, y+h);
	OUT_VERTEX_F(0);
	OUT_VERTEX_F(1);

	OUT_VERTEX(x, y);
	OUT_VERTEX_F(0);
	OUT_VERTEX_F(0);
}

static void
gen7_render_fill_done(struct sna *sna, const struct sna_fill_op *op)
{
	gen7_vertex_flush(sna);
	kgem_bo_destroy(&sna->kgem, op->base.src.bo);
	_kgem_set_mode(&sna->kgem, KGEM_RENDER);
}

static Bool
gen7_render_fill(struct sna *sna, uint8_t alu,
		 PixmapPtr dst, struct kgem_bo *dst_bo,
		 uint32_t color,
		 struct sna_fill_op *op)
{
	DBG(("%s: (alu=%d, color=%x)\n", __FUNCTION__, alu, color));

#if NO_FILL
	return sna_blt_fill(sna, alu,
			    dst_bo, dst->drawable.bitsPerPixel,
			    color,
			    op);
#endif

	if (sna->kgem.mode == KGEM_BLT &&
	    sna_blt_fill(sna, alu,
			 dst_bo, dst->drawable.bitsPerPixel,
			 color,
			 op))
		return TRUE;

	if (!(alu == GXcopy || alu == GXclear) ||
	    dst->drawable.width > GEN7_MAX_SIZE ||
	    dst->drawable.height > GEN7_MAX_SIZE)
		return sna_blt_fill(sna, alu,
				    dst_bo, dst->drawable.bitsPerPixel,
				    color,
				    op);

	if (alu == GXclear)
		color = 0;

	op->base.op = color == 0 ? PictOpClear : PictOpSrc;

	op->base.dst.pixmap = dst;
	op->base.dst.width  = dst->drawable.width;
	op->base.dst.height = dst->drawable.height;
	op->base.dst.format = sna_format_for_depth(dst->drawable.depth);
	op->base.dst.bo = dst_bo;

	op->base.src.bo =
		sna_render_get_solid(sna,
				     sna_rgba_for_color(color,
							dst->drawable.depth));
	op->base.src.filter = SAMPLER_FILTER_NEAREST;
	op->base.src.repeat = SAMPLER_EXTEND_REPEAT;

	op->base.is_affine = TRUE;
	op->base.floats_per_vertex = 3;

	op->base.u.gen7.wm_kernel = GEN7_WM_KERNEL_NOMASK;
	op->base.u.gen7.nr_surfaces = 2;
	op->base.u.gen7.nr_inputs = 1;
	op->base.u.gen7.ve_id = 1;

	if (!kgem_check_bo(&sna->kgem, dst_bo))
		kgem_submit(&sna->kgem);

	gen7_emit_fill_state(sna, &op->base);
	gen7_align_vertex(sna, &op->base);

	op->blt  = gen7_render_fill_blt;
	op->done = gen7_render_fill_done;
	return TRUE;
}

static void gen7_render_flush(struct sna *sna)
{
	gen7_vertex_finish(sna, TRUE);
}

static void
gen7_render_context_switch(struct kgem *kgem,
			   int new_mode)
{
	if (!new_mode)
		return;

	if (kgem->mode)
		_kgem_submit(kgem);

	kgem->ring = new_mode;
}

static void gen7_render_reset(struct sna *sna)
{
	sna->render_state.gen7.needs_invariant = TRUE;
	sna->render_state.gen7.vb_id = 0;
	sna->render_state.gen7.ve_id = -1;
	sna->render_state.gen7.last_primitive = -1;

	sna->render_state.gen7.num_sf_outputs = 0;
	sna->render_state.gen7.samplers = -1;
	sna->render_state.gen7.blend = -1;
	sna->render_state.gen7.kernel = -1;
	sna->render_state.gen7.drawrect_offset = -1;
	sna->render_state.gen7.drawrect_limit = -1;
	sna->render_state.gen7.surface_table = -1;
}

static void gen7_render_fini(struct sna *sna)
{
	kgem_bo_destroy(&sna->kgem, sna->render_state.gen7.general_bo);
}

static Bool gen7_render_setup(struct sna *sna)
{
	struct gen7_render_state *state = &sna->render_state.gen7;
	struct sna_static_stream general;
	struct gen7_sampler_state *ss;
	int i, j, k, l, m;

	sna_static_stream_init(&general);

	/* Zero pad the start. If you see an offset of 0x0 in the batchbuffer
	 * dumps, you know it points to zero.
	 */
	null_create(&general);

	for (m = 0; m < GEN7_KERNEL_COUNT; m++)
		state->wm_kernel[m] =
			sna_static_stream_add(&general,
					       wm_kernels[m].data,
					       wm_kernels[m].size,
					       64);

	ss = sna_static_stream_map(&general,
				   2 * sizeof(*ss) *
				   FILTER_COUNT * EXTEND_COUNT *
				   FILTER_COUNT * EXTEND_COUNT,
				   32);
	state->wm_state = sna_static_stream_offsetof(&general, ss);
	for (i = 0; i < FILTER_COUNT; i++) {
		for (j = 0; j < EXTEND_COUNT; j++) {
			for (k = 0; k < FILTER_COUNT; k++) {
				for (l = 0; l < EXTEND_COUNT; l++) {
					sampler_state_init(ss++, i, j);
					sampler_state_init(ss++, k, l);
				}
			}
		}
	}

	state->cc_vp = gen7_create_cc_viewport(&general);
	state->cc_blend = gen7_composite_create_blend_state(&general);

	state->general_bo = sna_static_stream_fini(sna, &general);
	return state->general_bo != NULL;
}

Bool gen7_render_init(struct sna *sna)
{
	if (!gen7_render_setup(sna))
		return FALSE;

	sna->kgem.context_switch = gen7_render_context_switch;

	sna->render.composite = gen7_render_composite;
	sna->render.video = gen7_render_video;

	sna->render.copy_boxes = gen7_render_copy_boxes;
	sna->render.copy = gen7_render_copy;

	sna->render.fill_boxes = gen7_render_fill_boxes;
	sna->render.fill = gen7_render_fill;

	sna->render.flush = gen7_render_flush;
	sna->render.reset = gen7_render_reset;
	sna->render.fini = gen7_render_fini;

	sna->render.max_3d_size = GEN7_MAX_SIZE;
	return TRUE;
}