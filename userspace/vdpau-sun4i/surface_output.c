// SPDX-License-Identifier: MIT
// Copyright (c) 2026 D.R.racer <drracer@seznam.cz>
/*
 * Output and bitmap surfaces. An output surface does not hold the video (the
 * mixer only records which video surface goes where, see mixer.c); its
 * pixels are the OSD layer shown on a separate overlay plane above the video.
 * That layer is straight-alpha ARGB8888 in a DRM dumb buffer; drawing
 * composites "over" it, with the colour blend factors the application passes
 * and coverage for alpha (VDPAU assumes an opaque destination, we don't have
 * one).
 */
#include <stdlib.h>
#include <string.h>

#include "vdpau_private.h"

#define MAX_SIZE	4096

static bool rgba_format_ok(VdpRGBAFormat f)
{
	return f == VDP_RGBA_FORMAT_B8G8R8A8 || f == VDP_RGBA_FORMAT_R8G8B8A8;
}

VdpStatus vdp_output_surface_query_capabilities(VdpDevice device, VdpRGBAFormat surface_rgba_format,
						VdpBool *is_supported, uint32_t *max_width,
						uint32_t *max_height)
{
	if (!is_supported || !max_width || !max_height)
		return VDP_STATUS_INVALID_POINTER;
	if (!handle_get(device))
		return VDP_STATUS_INVALID_HANDLE;
	*is_supported = rgba_format_ok(surface_rgba_format);
	*max_width = MAX_SIZE;
	*max_height = MAX_SIZE;
	return VDP_STATUS_OK;
}

VdpStatus vdp_output_surface_create(VdpDevice device, VdpRGBAFormat rgba_format, uint32_t width,
				    uint32_t height, VdpOutputSurface *surface)
{
	device_ctx_t *dev = handle_get(device);
	output_surface_ctx_t *os;

	if (!surface)
		return VDP_STATUS_INVALID_POINTER;
	if (!dev)
		return VDP_STATUS_INVALID_HANDLE;
	if (!rgba_format_ok(rgba_format))
		return VDP_STATUS_INVALID_RGBA_FORMAT;
	if (!width || !height || width > MAX_SIZE || height > MAX_SIZE)
		return VDP_STATUS_INVALID_SIZE;

	os = calloc(1, sizeof(*os));
	if (!os)
		return VDP_STATUS_RESOURCES;
	os->device = dev;
	os->format = rgba_format;
	os->width = width;
	os->height = height;
	os->status = VDP_PRESENTATION_QUEUE_STATUS_IDLE;
	os->video_buf = -1;

	*surface = handle_create(os);
	if (*surface == VDP_INVALID_HANDLE) {
		free(os);
		return VDP_STATUS_RESOURCES;
	}
	return VDP_STATUS_OK;
}

VdpStatus vdp_output_surface_destroy(VdpOutputSurface surface)
{
	output_surface_ctx_t *os = handle_get(surface);

	if (!os)
		return VDP_STATUS_INVALID_HANDLE;
	presentation_forget_surface(os->device, os);
	pthread_mutex_lock(&os->device->ref_lock);
	decoder_unref_buffer(os->device, os->video_buf, os->video_gen);
	pthread_mutex_unlock(&os->device->ref_lock);
	if (os->osd.handle)
		drm_dumb_destroy(os->device, &os->osd);
	free(os->shadow);
	handle_destroy(surface);
	free(os);
	return VDP_STATUS_OK;
}

VdpStatus vdp_output_surface_get_parameters(VdpOutputSurface surface, VdpRGBAFormat *rgba_format,
					    uint32_t *width, uint32_t *height)
{
	output_surface_ctx_t *os = handle_get(surface);

	if (!rgba_format || !width || !height)
		return VDP_STATUS_INVALID_POINTER;
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;
	*rgba_format = os->format;
	*width = os->width;
	*height = os->height;
	return VDP_STATUS_OK;
}

/* Make the OSD buffer exist and be cleared if the mixer asked for it. */
static int osd_prepare(output_surface_ctx_t *os)
{
	if (!os->osd.handle) {
		os->shadow = calloc((size_t)os->width * os->height, 4);
		if (!os->shadow)
			return -1;
		if (drm_open(os->device) ||
		    drm_dumb_create(os->device, os->width, os->height, &os->osd)) {
			free(os->shadow);
			os->shadow = NULL;
			return -1;
		}
		os->osd_clear = false;	/* both start zeroed */
	}
	if (os->osd_clear) {
		/* only what was drawn: a full 1080p clear is 8 MB of uncached writes */
		for (uint32_t y = os->osd_dirty.y0; y < os->osd_dirty.y1; y++) {
			size_t w = (os->osd_dirty.x1 - os->osd_dirty.x0) * 4;

			memset(os->shadow + (size_t)y * os->width + os->osd_dirty.x0, 0, w);
			memset((uint8_t *)os->osd.map + (size_t)y * os->osd.pitch + os->osd_dirty.x0 * 4,
			       0, w);
		}
		os->osd_dirty = (VdpRect){ 0, 0, 0, 0 };
		os->osd_clear = false;
	}
	return 0;
}

/* Copy a changed rectangle of the shadow to the scanned-out buffer. */
static void osd_flush(output_surface_ctx_t *os, const VdpRect *r)
{
	for (uint32_t y = r->y0; y < r->y1; y++)
		memcpy((uint8_t *)os->osd.map + (size_t)y * os->osd.pitch + r->x0 * 4,
		       os->shadow + (size_t)y * os->width + r->x0, (r->x1 - r->x0) * 4);
}

static void osd_mark(output_surface_ctx_t *os, const VdpRect *r)
{
	if (os->osd_dirty.x0 >= os->osd_dirty.x1) {
		os->osd_dirty = *r;
		return;
	}
	if (r->x0 < os->osd_dirty.x0)
		os->osd_dirty.x0 = r->x0;
	if (r->y0 < os->osd_dirty.y0)
		os->osd_dirty.y0 = r->y0;
	if (r->x1 > os->osd_dirty.x1)
		os->osd_dirty.x1 = r->x1;
	if (r->y1 > os->osd_dirty.y1)
		os->osd_dirty.y1 = r->y1;
}

static inline uint32_t *osd_px(output_surface_ctx_t *os, uint32_t x, uint32_t y)
{
	return os->shadow + (size_t)y * os->width + x;
}

/* ARGB8888 word <-> the surface's RGBA format in memory (little endian) */
static uint32_t to_argb(VdpRGBAFormat f, uint32_t v)
{
	if (f == VDP_RGBA_FORMAT_R8G8B8A8)	/* bytes R,G,B,A -> word 0xAABBGGRR */
		return (v & 0xff00ff00) | ((v & 0xff) << 16) | ((v >> 16) & 0xff);
	return v;
}

VdpStatus vdp_output_surface_put_bits_native(VdpOutputSurface surface, void const *const *source_data,
					     uint32_t const *source_pitches, VdpRect const *destination_rect)
{
	output_surface_ctx_t *os = handle_get(surface);
	VdpRect r;

	if (!source_data || !source_pitches)
		return VDP_STATUS_INVALID_POINTER;
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;
	r = destination_rect ? *destination_rect : (VdpRect){ 0, 0, os->width, os->height };
	if (r.x1 > os->width || r.y1 > os->height || r.x0 > r.x1 || r.y0 > r.y1)
		return VDP_STATUS_INVALID_SIZE;
	if (osd_prepare(os))
		return VDP_STATUS_RESOURCES;

	for (uint32_t y = r.y0; y < r.y1; y++) {
		const uint32_t *src = (const uint32_t *)((const uint8_t *)source_data[0] +
							 (size_t)(y - r.y0) * source_pitches[0]);
		for (uint32_t x = r.x0; x < r.x1; x++)
			*osd_px(os, x, y) = to_argb(os->format, src[x - r.x0]);
	}
	osd_flush(os, &r);
	osd_mark(os, &r);
	os->osd_used = true;
	return VDP_STATUS_OK;
}

VdpStatus vdp_output_surface_get_bits_native(VdpOutputSurface surface, VdpRect const *source_rect,
					     void *const *destination_data,
					     uint32_t const *destination_pitches)
{
	output_surface_ctx_t *os = handle_get(surface);
	VdpRect r;

	if (!destination_data || !destination_pitches)
		return VDP_STATUS_INVALID_POINTER;
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;
	r = source_rect ? *source_rect : (VdpRect){ 0, 0, os->width, os->height };
	if (r.x1 > os->width || r.y1 > os->height || r.x0 > r.x1 || r.y0 > r.y1)
		return VDP_STATUS_INVALID_SIZE;

	/* Only the OSD layer exists here; the video is never composited in memory. */
	for (uint32_t y = r.y0; y < r.y1; y++) {
		uint32_t *dst = (uint32_t *)((uint8_t *)destination_data[0] +
					     (size_t)(y - r.y0) * destination_pitches[0]);
		for (uint32_t x = r.x0; x < r.x1; x++)
			dst[x - r.x0] = os->osd_used && !os->osd_clear ?
					to_argb(os->format, *osd_px(os, x, y)) : 0;
	}
	return VDP_STATUS_OK;
}

VdpStatus vdp_output_surface_put_bits_indexed(VdpOutputSurface surface, VdpIndexedFormat source_indexed_format,
					      void const *const *source_data, uint32_t const *source_pitch,
					      VdpRect const *destination_rect, VdpColorTableFormat color_table_format,
					      void const *color_table)
{
	return handle_get(surface) ? VDP_STATUS_NO_IMPLEMENTATION : VDP_STATUS_INVALID_HANDLE;
}

/* --- bitmap surfaces --- */

VdpStatus vdp_bitmap_surface_query_capabilities(VdpDevice device, VdpRGBAFormat surface_rgba_format,
						VdpBool *is_supported, uint32_t *max_width,
						uint32_t *max_height)
{
	if (!is_supported || !max_width || !max_height)
		return VDP_STATUS_INVALID_POINTER;
	if (!handle_get(device))
		return VDP_STATUS_INVALID_HANDLE;
	*is_supported = rgba_format_ok(surface_rgba_format) || surface_rgba_format == VDP_RGBA_FORMAT_A8;
	*max_width = MAX_SIZE;
	*max_height = MAX_SIZE;
	return VDP_STATUS_OK;
}

VdpStatus vdp_bitmap_surface_create(VdpDevice device, VdpRGBAFormat rgba_format, uint32_t width,
				    uint32_t height, VdpBool frequently_accessed, VdpBitmapSurface *surface)
{
	bitmap_surface_ctx_t *bs;

	if (!surface)
		return VDP_STATUS_INVALID_POINTER;
	if (!handle_get(device))
		return VDP_STATUS_INVALID_HANDLE;
	if (!rgba_format_ok(rgba_format) && rgba_format != VDP_RGBA_FORMAT_A8)
		return VDP_STATUS_INVALID_RGBA_FORMAT;
	if (!width || !height || width > MAX_SIZE || height > MAX_SIZE)
		return VDP_STATUS_INVALID_SIZE;

	bs = calloc(1, sizeof(*bs));
	if (!bs)
		return VDP_STATUS_RESOURCES;
	bs->format = rgba_format;
	bs->width = width;
	bs->height = height;
	bs->bpp = rgba_format == VDP_RGBA_FORMAT_A8 ? 1 : 4;
	bs->data = calloc((size_t)width * height, bs->bpp);
	if (!bs->data) {
		free(bs);
		return VDP_STATUS_RESOURCES;
	}
	*surface = handle_create(bs);
	if (*surface == VDP_INVALID_HANDLE) {
		free(bs->data);
		free(bs);
		return VDP_STATUS_RESOURCES;
	}
	return VDP_STATUS_OK;
}

VdpStatus vdp_bitmap_surface_destroy(VdpBitmapSurface surface)
{
	bitmap_surface_ctx_t *bs = handle_get(surface);

	if (!bs)
		return VDP_STATUS_INVALID_HANDLE;
	handle_destroy(surface);
	free(bs->data);
	free(bs);
	return VDP_STATUS_OK;
}

VdpStatus vdp_bitmap_surface_put_bits_native(VdpBitmapSurface surface, void const *const *source_data,
					     uint32_t const *source_pitches, VdpRect const *destination_rect)
{
	bitmap_surface_ctx_t *bs = handle_get(surface);
	VdpRect r;

	if (!source_data || !source_pitches)
		return VDP_STATUS_INVALID_POINTER;
	if (!bs)
		return VDP_STATUS_INVALID_HANDLE;
	r = destination_rect ? *destination_rect : (VdpRect){ 0, 0, bs->width, bs->height };
	if (r.x1 > bs->width || r.y1 > bs->height || r.x0 > r.x1 || r.y0 > r.y1)
		return VDP_STATUS_INVALID_SIZE;
	for (uint32_t y = r.y0; y < r.y1; y++)
		memcpy(bs->data + ((size_t)y * bs->width + r.x0) * bs->bpp,
		       (const uint8_t *)source_data[0] + (size_t)(y - r.y0) * source_pitches[0],
		       (size_t)(r.x1 - r.x0) * bs->bpp);
	return VDP_STATUS_OK;
}

/* --- rendering --- */

/* Source pixel as normalised-to-255 straight r, g, b, a. */
struct px {
	uint32_t r, g, b, a;
};

static float factor(VdpOutputSurfaceRenderBlendFactor f, float src_a)
{
	switch (f) {
	case VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ZERO:
		return 0.0f;
	case VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_SRC_ALPHA:
		return src_a;
	case VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
		return 1.0f - src_a;
	default:	/* ONE and anything unsupported */
		return 1.0f;
	}
}

/*
 * Composite one source pixel over the straight-alpha destination pixel:
 * premultiplied source colour = colour * source blend factor, coverage =
 * source alpha; then classic "over". Without a blend state the source
 * replaces the destination.
 */
static uint32_t blend(uint32_t dst, struct px s, const VdpOutputSurfaceRenderBlendState *bs)
{
	float sa = s.a / 255.0f, fs, da, dr, dg, db, oa;

	if (!bs)
		return (s.a << 24) | (s.r << 16) | (s.g << 8) | s.b;

	fs = factor(bs->blend_factor_source_color, sa);
	da = (dst >> 24) / 255.0f;
	/* destination premultiplied */
	dr = ((dst >> 16) & 0xff) * da;
	dg = ((dst >> 8) & 0xff) * da;
	db = (dst & 0xff) * da;
	oa = sa + da * (1.0f - sa);
	if (oa <= 0.0f)
		return 0;
	dr = (s.r * fs + dr * (1.0f - sa)) / oa;
	dg = (s.g * fs + dg * (1.0f - sa)) / oa;
	db = (s.b * fs + db * (1.0f - sa)) / oa;
#define CLAMP255(v) ((uint32_t)((v) > 255.0f ? 255.0f : (v) < 0.0f ? 0.0f : (v) + 0.5f))
	return (CLAMP255(oa * 255.0f) << 24) | (CLAMP255(dr) << 16) | (CLAMP255(dg) << 8) | CLAMP255(db);
#undef CLAMP255
}

static struct px modulate(struct px p, const VdpColor *c)
{
	if (!c)
		return p;
	return (struct px){
		.r = (uint32_t)(p.r * c->red + 0.5f),
		.g = (uint32_t)(p.g * c->green + 0.5f),
		.b = (uint32_t)(p.b * c->blue + 0.5f),
		.a = (uint32_t)(p.a * c->alpha + 0.5f),
	};
}

typedef struct px (*fetch_fn)(const void *src, uint32_t x, uint32_t y);

static struct px fetch_bitmap(const void *src, uint32_t x, uint32_t y)
{
	const bitmap_surface_ctx_t *bs = src;
	const uint8_t *p = bs->data + ((size_t)y * bs->width + x) * bs->bpp;

	switch (bs->format) {
	case VDP_RGBA_FORMAT_A8:
		return (struct px){ 255, 255, 255, p[0] };
	case VDP_RGBA_FORMAT_R8G8B8A8:
		return (struct px){ p[0], p[1], p[2], p[3] };
	default:	/* B8G8R8A8 */
		return (struct px){ p[2], p[1], p[0], p[3] };
	}
}

static struct px fetch_output(const void *src, uint32_t x, uint32_t y)
{
	output_surface_ctx_t *os = (output_surface_ctx_t *)src;
	uint32_t v;

	if (!os->osd_used || os->osd_clear)
		return (struct px){ 0, 0, 0, 0 };
	v = *osd_px(os, x, y);
	return (struct px){ (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, v >> 24 };
}

static VdpStatus render(output_surface_ctx_t *dst, VdpRect const *dst_rect, const void *src,
			uint32_t src_w, uint32_t src_h, fetch_fn fetch, VdpRect const *src_rect,
			VdpColor const *colors, VdpOutputSurfaceRenderBlendState const *bs,
			uint32_t flags)
{
	VdpRect d = dst_rect ? *dst_rect : (VdpRect){ 0, 0, dst->width, dst->height };
	VdpRect s = src_rect ? *src_rect : (VdpRect){ 0, 0, src_w, src_h };
	uint32_t dw, dh, sw, sh;

	if ((flags & 3) != VDP_OUTPUT_SURFACE_RENDER_ROTATE_0)
		return VDP_STATUS_NO_IMPLEMENTATION;
	if (d.x1 > dst->width)
		d.x1 = dst->width;
	if (d.y1 > dst->height)
		d.y1 = dst->height;
	if (d.x0 >= d.x1 || d.y0 >= d.y1)
		return VDP_STATUS_OK;
	if (osd_prepare(dst))
		return VDP_STATUS_RESOURCES;

	dw = d.x1 - d.x0;
	dh = d.y1 - d.y0;
	if (vdpau_debug) {
		static unsigned long calls, pixels;

		calls++;
		pixels += (unsigned long)dw * dh;
		if (!(calls % 200))
			DBG("render: %lu calls, %lu pixels (last %ux%u from %ux%u, src %s)\n", calls,
			    pixels, dw, dh, s.x1 - s.x0, s.y1 - s.y0, src ? "yes" : "none");
	}
	sw = s.x1 > s.x0 ? s.x1 - s.x0 : 0;
	sh = s.y1 > s.y0 ? s.y1 - s.y0 : 0;
	{
		/* source column per destination column (nearest), computed once */
		uint32_t *xmap = malloc(dw * sizeof(*xmap));
		const VdpColor *color = (flags & VDP_OUTPUT_SURFACE_RENDER_COLOR_PER_VERTEX) ?
					&colors[0] : colors;
		/* transparent source pixels change nothing when colour is scaled by alpha */
		bool skip_clear = bs && bs->blend_factor_source_color ==
					VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_SRC_ALPHA;

		if (!xmap)
			return VDP_STATUS_RESOURCES;
		for (uint32_t x = 0; x < dw; x++)
			xmap[x] = s.x0 + (sw == dw ? x : sw ? (uint32_t)((uint64_t)x * sw / dw) : 0);

		for (uint32_t y = 0; y < dh; y++) {
			uint32_t sy = s.y0 + (sh == dh ? y : sh ? (uint32_t)((uint64_t)y * sh / dh) : 0);
			uint32_t *dp = osd_px(dst, d.x0, d.y0 + y);

			for (uint32_t x = 0; x < dw; x++) {
				/* no source: an opaque white pixel (VDPAU) */
				struct px p = src ? fetch(src, xmap[x], sy) :
						    (struct px){ 255, 255, 255, 255 };

				if (!p.a && (skip_clear || !(p.r | p.g | p.b)) && bs)
					continue;
				dp[x] = blend(dp[x], modulate(p, color), bs);
			}
		}
		free(xmap);
	}
	osd_flush(dst, &d);
	osd_mark(dst, &d);
	dst->osd_used = true;
	return VDP_STATUS_OK;
}

VdpStatus vdp_output_surface_render_output_surface(VdpOutputSurface destination_surface,
						   VdpRect const *destination_rect,
						   VdpOutputSurface source_surface, VdpRect const *source_rect,
						   VdpColor const *colors,
						   VdpOutputSurfaceRenderBlendState const *blend_state,
						   uint32_t flags)
{
	output_surface_ctx_t *dst = handle_get(destination_surface);
	output_surface_ctx_t *src = NULL;

	if (!dst)
		return VDP_STATUS_INVALID_HANDLE;
	if (source_surface != VDP_INVALID_HANDLE) {
		src = handle_get(source_surface);
		if (!src)
			return VDP_STATUS_INVALID_HANDLE;
	}
	return render(dst, destination_rect, src, src ? src->width : 1, src ? src->height : 1,
		      fetch_output, source_rect, colors, blend_state, flags);
}

VdpStatus vdp_output_surface_render_bitmap_surface(VdpOutputSurface destination_surface,
						   VdpRect const *destination_rect,
						   VdpBitmapSurface source_surface, VdpRect const *source_rect,
						   VdpColor const *colors,
						   VdpOutputSurfaceRenderBlendState const *blend_state,
						   uint32_t flags)
{
	output_surface_ctx_t *dst = handle_get(destination_surface);
	bitmap_surface_ctx_t *src = NULL;

	if (!dst)
		return VDP_STATUS_INVALID_HANDLE;
	if (source_surface != VDP_INVALID_HANDLE) {
		src = handle_get(source_surface);
		if (!src)
			return VDP_STATUS_INVALID_HANDLE;
	}
	return render(dst, destination_rect, src, src ? src->width : 1, src ? src->height : 1,
		      fetch_bitmap, source_rect, colors, blend_state, flags);
}
