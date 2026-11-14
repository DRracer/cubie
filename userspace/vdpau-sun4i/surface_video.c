// SPDX-License-Identifier: MIT
// Copyright (c) 2026 D.R.racer <drracer@seznam.cz>
/* VDPAU video surfaces: cedrus CAPTURE buffers (NV12, 32x32 tiles). */
#include <stdlib.h>
#include <string.h>

#include "vdpau_private.h"

#define ALIGN32(x)	(((x) + 31) & ~31U)

VdpStatus vdp_video_surface_query_capabilities(VdpDevice device, VdpChromaType surface_chroma_type,
					       VdpBool *is_supported, uint32_t *max_width,
					       uint32_t *max_height)
{
	if (!is_supported || !max_width || !max_height)
		return VDP_STATUS_INVALID_POINTER;
	if (!handle_get(device))
		return VDP_STATUS_INVALID_HANDLE;
	*is_supported = surface_chroma_type == VDP_CHROMA_TYPE_420;
	*max_width = 2048;
	*max_height = 2048;
	return VDP_STATUS_OK;
}

VdpStatus vdp_video_surface_query_ycbcr_capabilities(VdpDevice device,
						     VdpChromaType surface_chroma_type,
						     VdpYCbCrFormat bits_ycbcr_format,
						     VdpBool *is_supported)
{
	if (!is_supported)
		return VDP_STATUS_INVALID_POINTER;
	if (!handle_get(device))
		return VDP_STATUS_INVALID_HANDLE;
	/* Read-back only (GetBits); PutBits is not supported. */
	*is_supported = surface_chroma_type == VDP_CHROMA_TYPE_420 &&
			(bits_ycbcr_format == VDP_YCBCR_FORMAT_NV12 ||
			 bits_ycbcr_format == VDP_YCBCR_FORMAT_YV12);
	return VDP_STATUS_OK;
}

VdpStatus vdp_video_surface_create(VdpDevice device, VdpChromaType chroma_type, uint32_t width,
				   uint32_t height, VdpVideoSurface *surface)
{
	device_ctx_t *dev = handle_get(device);
	video_surface_ctx_t *vs;

	if (!surface)
		return VDP_STATUS_INVALID_POINTER;
	if (!dev)
		return VDP_STATUS_INVALID_HANDLE;
	if (chroma_type != VDP_CHROMA_TYPE_420)
		return VDP_STATUS_INVALID_CHROMA_TYPE;
	if (!width || !height || width > 2048 || height > 2048)
		return VDP_STATUS_INVALID_SIZE;

	vs = calloc(1, sizeof(*vs));
	if (!vs)
		return VDP_STATUS_RESOURCES;
	vs->device = dev;
	vs->chroma_type = chroma_type;
	vs->width = width;
	vs->height = height;
	vs->index = -1;		/* bound to a CAPTURE buffer at first decode */

	*surface = handle_create(vs);
	if (*surface == VDP_INVALID_HANDLE) {
		free(vs);
		return VDP_STATUS_RESOURCES;
	}
	return VDP_STATUS_OK;
}

VdpStatus vdp_video_surface_destroy(VdpVideoSurface surface)
{
	video_surface_ctx_t *vs = handle_get(surface);

	if (!vs)
		return VDP_STATUS_INVALID_HANDLE;
	pthread_mutex_lock(&vs->device->ref_lock);
	if (vs->device->decoder)
		decoder_release_surface(vs->device->decoder, vs);
	pthread_mutex_unlock(&vs->device->ref_lock);
	handle_destroy(surface);
	free(vs);
	return VDP_STATUS_OK;
}

VdpStatus vdp_video_surface_get_parameters(VdpVideoSurface surface, VdpChromaType *chroma_type,
					   uint32_t *width, uint32_t *height)
{
	video_surface_ctx_t *vs = handle_get(surface);

	if (!chroma_type || !width || !height)
		return VDP_STATUS_INVALID_POINTER;
	if (!vs)
		return VDP_STATUS_INVALID_HANDLE;
	*chroma_type = vs->chroma_type;
	*width = vs->width;
	*height = vs->height;
	return VDP_STATUS_OK;
}

/*
 * The destinations are sized for the surface (FFmpeg allocates the frames
 * context size); rows/columns beyond it are never written.
 *
 * NV12_32L32: 32x32-byte tiles, tile rows of `stride * 32` bytes; the
 * interleaved CbCr plane follows the 32-aligned luma plane in the same layout.
 */
static void detile(const uint8_t *src, unsigned int stride, uint8_t *dst, unsigned int pitch,
		   unsigned int width, unsigned int height)
{
	for (unsigned int y = 0; y < height; y++) {
		const uint8_t *row = src + (y / 32) * stride * 32 + (y % 32) * 32;
		uint8_t *d = dst + (size_t)y * pitch;

		for (unsigned int x = 0; x < width; x += 32)
			memcpy(d + x, row + (x / 32) * 1024, width - x < 32 ? width - x : 32);
	}
}

static void detile_split_uv(const uint8_t *src, unsigned int stride, uint8_t *u, uint8_t *v,
			    unsigned int pitch, unsigned int width, unsigned int height)
{
	/* width/height of the chroma planes (samples) */
	for (unsigned int y = 0; y < height; y++) {
		const uint8_t *row = src + (y / 32) * stride * 32 + (y % 32) * 32;

		for (unsigned int x = 0; x < width; x++) {
			unsigned int b = 2 * x;
			const uint8_t *p = row + (b / 32) * 1024 + b % 32;

			u[(size_t)y * pitch + x] = p[0];
			v[(size_t)y * pitch + x] = p[1];
		}
	}
}

static VdpStatus get_bits(decoder_ctx_t *dec, video_surface_ctx_t *vs,
			  VdpYCbCrFormat destination_ycbcr_format, void *const *destination_data,
			  uint32_t const *destination_pitches)
{
	const uint8_t *luma, *chroma;
	unsigned int w, h;

	if (decoder_surface_picture(dec, vs) < 0) {
		DBG("GetBits on a surface without a decoded picture\n");
		return VDP_STATUS_ERROR;
	}

	w = vs->width < dec->cap_width ? vs->width : dec->cap_width;
	h = vs->height < dec->cap_height ? vs->height : dec->cap_height;
	luma = dec->capture[vs->index].map;
	chroma = luma + (size_t)dec->cap_stride * ALIGN32(dec->cap_height);

	switch (destination_ycbcr_format) {
	case VDP_YCBCR_FORMAT_NV12:
		detile(luma, dec->cap_stride, destination_data[0], destination_pitches[0], w, h);
		/* interleaved CbCr: round odd widths up to whole sample pairs */
		detile(chroma, dec->cap_stride, destination_data[1], destination_pitches[1],
		       (w + 1) & ~1U, (h + 1) / 2);
		return VDP_STATUS_OK;
	case VDP_YCBCR_FORMAT_YV12:
		if (destination_pitches[1] != destination_pitches[2])
			return VDP_STATUS_ERROR;
		detile(luma, dec->cap_stride, destination_data[0], destination_pitches[0], w, h);
		/* YV12: plane 1 = Cr (V), plane 2 = Cb (U) */
		detile_split_uv(chroma, dec->cap_stride, destination_data[2], destination_data[1],
				destination_pitches[1], (w + 1) / 2, (h + 1) / 2);
		return VDP_STATUS_OK;
	default:
		return VDP_STATUS_INVALID_Y_CB_CR_FORMAT;
	}
}

VdpStatus vdp_video_surface_get_bits_ycbcr(VdpVideoSurface surface,
					   VdpYCbCrFormat destination_ycbcr_format,
					   void *const *destination_data,
					   uint32_t const *destination_pitches)
{
	video_surface_ctx_t *vs = handle_get(surface);
	device_ctx_t *dev;
	VdpStatus st;

	if (!destination_data || !destination_pitches)
		return VDP_STATUS_INVALID_POINTER;
	if (!vs)
		return VDP_STATUS_INVALID_HANDLE;
	dev = vs->device;
	/* the decoder must not be destroyed while its CAPTURE buffer is read */
	pthread_mutex_lock(&dev->decoder_lock);
	decoder_sync(dev);
	st = get_bits(dev->decoder, vs, destination_ycbcr_format, destination_data,
		      destination_pitches);
	pthread_mutex_unlock(&dev->decoder_lock);
	return st;
}

VdpStatus vdp_video_surface_put_bits_ycbcr(VdpVideoSurface surface,
					   VdpYCbCrFormat source_ycbcr_format,
					   void const *const *source_data,
					   uint32_t const *source_pitches)
{
	(void)source_ycbcr_format;
	(void)source_data;
	(void)source_pitches;
	return handle_get(surface) ? VDP_STATUS_NO_IMPLEMENTATION : VDP_STATUS_INVALID_HANDLE;
}
