// SPDX-License-Identifier: MIT
// Copyright (c) 2026 D.R.racer <drracer@seznam.cz>
/*
 * Video mixer: no pixel processing. Rendering records which video surface
 * goes where in the output surface; the presentation queue shows it on the
 * video plane, where the display frontend converts and scales it. Features
 * (deinterlacing, noise reduction, sharpening, ...) are reported unsupported;
 * if an application requests them anyway they are ignored rather than failing
 * its mixer creation.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vdpau_private.h"

VdpStatus vdp_video_mixer_query_feature_support(VdpDevice device, VdpVideoMixerFeature feature,
						VdpBool *is_supported)
{
	if (!is_supported)
		return VDP_STATUS_INVALID_POINTER;
	if (!handle_get(device))
		return VDP_STATUS_INVALID_HANDLE;
	*is_supported = VDP_FALSE;
	return VDP_STATUS_OK;
}

VdpStatus vdp_video_mixer_create(VdpDevice device, uint32_t feature_count,
				 VdpVideoMixerFeature const *features, uint32_t parameter_count,
				 VdpVideoMixerParameter const *parameters,
				 void const *const *parameter_values, VdpVideoMixer *mixer)
{
	device_ctx_t *dev = handle_get(device);
	video_mixer_ctx_t *mix;

	if (!mixer)
		return VDP_STATUS_INVALID_POINTER;
	if (!dev)
		return VDP_STATUS_INVALID_HANDLE;
	/* Features (reported unsupported) are accepted and ignored. */

	mix = calloc(1, sizeof(*mix));
	if (!mix)
		return VDP_STATUS_RESOURCES;
	mix->device = dev;
	for (uint32_t i = 0; i < parameter_count; i++) {
		if (parameters[i] == VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH)
			mix->width = *(const uint32_t *)parameter_values[i];
		else if (parameters[i] == VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT)
			mix->height = *(const uint32_t *)parameter_values[i];
	}
	*mixer = handle_create(mix);
	if (*mixer == VDP_INVALID_HANDLE) {
		free(mix);
		return VDP_STATUS_RESOURCES;
	}
	return VDP_STATUS_OK;
}

VdpStatus vdp_video_mixer_destroy(VdpVideoMixer mixer)
{
	video_mixer_ctx_t *mix = handle_get(mixer);

	if (!mix)
		return VDP_STATUS_INVALID_HANDLE;
	handle_destroy(mixer);
	free(mix);
	return VDP_STATUS_OK;
}

VdpStatus vdp_video_mixer_set_feature_enables(VdpVideoMixer mixer, uint32_t feature_count,
					      VdpVideoMixerFeature const *features,
					      VdpBool const *feature_enables)
{
	/* Nothing to enable: features are reported unsupported and ignored. */
	return handle_get(mixer) ? VDP_STATUS_OK : VDP_STATUS_INVALID_HANDLE;
}

/* CSC matrix, background colour, ...: the display engine does its own CSC. */
VdpStatus vdp_video_mixer_set_attribute_values(VdpVideoMixer mixer, uint32_t attribute_count,
					       VdpVideoMixerAttribute const *attributes,
					       void const *const *attribute_values)
{
	return handle_get(mixer) ? VDP_STATUS_OK : VDP_STATUS_INVALID_HANDLE;
}

VdpStatus vdp_video_mixer_render(VdpVideoMixer mixer, VdpOutputSurface background_surface,
				 VdpRect const *background_source_rect,
				 VdpVideoMixerPictureStructure current_picture_structure,
				 uint32_t video_surface_past_count, VdpVideoSurface const *video_surface_past,
				 VdpVideoSurface video_surface_current, uint32_t video_surface_future_count,
				 VdpVideoSurface const *video_surface_future,
				 VdpRect const *video_source_rect, VdpOutputSurface destination_surface,
				 VdpRect const *destination_rect, VdpRect const *destination_video_rect,
				 uint32_t layer_count, VdpLayer const *layers)
{
	video_mixer_ctx_t *mix = handle_get(mixer);
	output_surface_ctx_t *os = handle_get(destination_surface);
	video_surface_ctx_t *vs = handle_get(video_surface_current);

	if (!mix || !os || !vs)
		return VDP_STATUS_INVALID_HANDLE;
	if (layer_count)
		return VDP_STATUS_INVALID_VALUE;	/* no layers supported */

	/* reference the picture itself: the surface may be decoded into again */
	pthread_mutex_lock(&mix->device->decoder_lock);
	decoder_sync(mix->device);	/* the worker needs a finished picture */
	pthread_mutex_lock(&mix->device->ref_lock);
	{
		decoder_ctx_t *dec = mix->device->decoder;
		int buf = decoder_surface_picture(dec, vs);

		decoder_unref_buffer(mix->device, os->video_buf, os->video_gen);
		os->video_buf = buf;
		if (buf >= 0) {
			decoder_ref_buffer(dec, buf);
			os->video_gen = dec->generation;
		}
	}
	pthread_mutex_unlock(&mix->device->ref_lock);
	pthread_mutex_unlock(&mix->device->decoder_lock);
	os->video_src = video_source_rect ? *video_source_rect :
					    (VdpRect){ 0, 0, vs->width, vs->height };
	os->video_dst = destination_video_rect ? *destination_video_rect :
			destination_rect ? *destination_rect :
					   (VdpRect){ 0, 0, os->width, os->height };
	/* The mixer output replaces the surface: the OSD drawn later starts empty. */
	os->osd_used = false;
	os->osd_clear = true;
	return VDP_STATUS_OK;
}

/* VDPAU's procamp + colour standard -> 3x4 YCbCr->RGB matrix (studio range). */
VdpStatus vdp_generate_csc_matrix(VdpProcamp *procamp, VdpColorStandard standard,
				  VdpCSCMatrix *csc_matrix)
{
	float kr, kb, kg, uvcos = 1.0f, uvsin = 0.0f, contrast = 1.0f, brightness = 0.0f, saturation = 1.0f;
	float yr, cbg, cbb, crr, crg;

	if (!csc_matrix)
		return VDP_STATUS_INVALID_POINTER;

	switch (standard) {
	case VDP_COLOR_STANDARD_ITUR_BT_601:
		kr = 0.299f; kb = 0.114f;
		break;
	case VDP_COLOR_STANDARD_ITUR_BT_709:
		kr = 0.2126f; kb = 0.0722f;
		break;
	case VDP_COLOR_STANDARD_SMPTE_240M:
		kr = 0.212f; kb = 0.087f;
		break;
	default:
		return VDP_STATUS_INVALID_COLOR_STANDARD;
	}
	kg = 1.0f - kr - kb;
	if (procamp) {
		if (procamp->struct_version > VDP_PROCAMP_VERSION)
			return VDP_STATUS_INVALID_STRUCT_VERSION;
		brightness = procamp->brightness;
		contrast = procamp->contrast;
		saturation = procamp->saturation;
		uvcos = saturation * cosf(procamp->hue);
		uvsin = saturation * sinf(procamp->hue);
	}

	yr = 255.0f / 219.0f * contrast;
	crr = 255.0f / 112.0f * (1.0f - kr) * contrast;
	cbb = 255.0f / 112.0f * (1.0f - kb) * contrast;
	cbg = -255.0f / 112.0f * (1.0f - kb) * kb / kg * contrast;
	crg = -255.0f / 112.0f * (1.0f - kr) * kr / kg * contrast;

	/* rows R, G, B; columns Y, Cb, Cr, offset */
	(*csc_matrix)[0][0] = yr;
	(*csc_matrix)[0][1] = -uvsin * crr;
	(*csc_matrix)[0][2] = uvcos * crr;
	(*csc_matrix)[1][0] = yr;
	(*csc_matrix)[1][1] = uvcos * cbg + uvsin * crg;
	(*csc_matrix)[1][2] = uvcos * crg - uvsin * cbg;
	(*csc_matrix)[2][0] = yr;
	(*csc_matrix)[2][1] = uvcos * cbb;
	(*csc_matrix)[2][2] = uvsin * cbb;
	for (int i = 0; i < 3; i++)
		(*csc_matrix)[i][3] = brightness - (*csc_matrix)[i][0] * 16.0f / 255.0f -
				      ((*csc_matrix)[i][1] + (*csc_matrix)[i][2]) * 128.0f / 255.0f;
	return VDP_STATUS_OK;
}
