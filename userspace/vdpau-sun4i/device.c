// SPDX-License-Identifier: MIT
// Copyright (c) 2026 D.R.racer <drracer@seznam.cz>
/* VDPAU device: entry point, function table, cedrus node discovery. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>
#include <vdpau/vdpau_x11.h>

#include "vdpau_private.h"

int vdpau_debug;

/* The cedrus node that decodes H.264 (the encoder node has no H264_SLICE OUTPUT). */
static int open_decoder_node(void)
{
	char path[32];

	for (int i = 0; i < 16; i++) {
		struct v4l2_capability cap = { 0 };
		int fd;

		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!ioctl(fd, VIDIOC_QUERYCAP, &cap) &&
		    !strcmp((char *)cap.driver, "cedrus")) {
			for (int j = 0;; j++) {
				struct v4l2_fmtdesc f = {
					.index = j, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
				};

				if (ioctl(fd, VIDIOC_ENUM_FMT, &f))
					break;
				if (f.pixelformat == V4L2_PIX_FMT_H264_SLICE) {
					DBG("decoder node %s\n", path);
					return fd;
				}
			}
		}
		close(fd);
	}
	return -1;
}

static int open_media_node(void)
{
	char path[32];

	for (int i = 0; i < 16; i++) {
		struct media_device_info info = { 0 };
		int fd;

		snprintf(path, sizeof(path), "/dev/media%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) &&
		    !strcmp(info.driver, "cedrus")) {
			DBG("media node %s\n", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

/* The only symbol libvdpau looks up in a driver. */
__attribute__((visibility("default")))
VdpStatus vdp_imp_device_create_x11(Display *display, int screen, VdpDevice *device,
				    VdpGetProcAddress **get_proc_address);

VdpStatus vdp_imp_device_create_x11(Display *display, int screen, VdpDevice *device,
				    VdpGetProcAddress **get_proc_address)
{
	device_ctx_t *dev;

	if (!device || !get_proc_address)
		return VDP_STATUS_INVALID_POINTER;

	vdpau_debug = getenv("VDPAU_SUN4I_DEBUG") != NULL;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return VDP_STATUS_RESOURCES;
	dev->display = display;
	dev->screen = screen;
	dev->drm.fd = -1;
	pthread_mutex_init(&dev->drm_lock, NULL);
	pthread_mutex_init(&dev->ref_lock, NULL);
	pthread_mutex_init(&dev->decoder_lock, NULL);
	dev->video_fd = open_decoder_node();
	dev->media_fd = open_media_node();
	if (dev->video_fd < 0 || dev->media_fd < 0) {
		ERR("no cedrus decoder (video %d, media %d)\n", dev->video_fd, dev->media_fd);
		if (dev->video_fd >= 0)
			close(dev->video_fd);
		if (dev->media_fd >= 0)
			close(dev->media_fd);
		free(dev);
		return VDP_STATUS_ERROR;
	}

	*device = handle_create(dev);
	if (*device == VDP_INVALID_HANDLE) {
		close(dev->video_fd);
		close(dev->media_fd);
		free(dev);
		return VDP_STATUS_RESOURCES;
	}
	*get_proc_address = vdp_get_proc_address;
	return VDP_STATUS_OK;
}

static VdpStatus vdp_device_destroy(VdpDevice device)
{
	device_ctx_t *dev = handle_get(device);

	if (!dev)
		return VDP_STATUS_INVALID_HANDLE;
	if (dev->decoder)
		return VDP_STATUS_ERROR;	/* decoder must be destroyed first */
	close(dev->video_fd);
	close(dev->media_fd);
	drm_close(dev);
	pthread_mutex_destroy(&dev->drm_lock);
	pthread_mutex_destroy(&dev->ref_lock);
	pthread_mutex_destroy(&dev->decoder_lock);
	handle_destroy(device);
	free(dev);
	return VDP_STATUS_OK;
}

static const char *vdp_get_error_string(VdpStatus status)
{
	switch (status) {
	case VDP_STATUS_OK: return "The operation completed successfully; no error.";
	case VDP_STATUS_NO_IMPLEMENTATION: return "No backend implementation could be loaded.";
	case VDP_STATUS_INVALID_HANDLE: return "An invalid handle value was provided.";
	case VDP_STATUS_INVALID_POINTER: return "An invalid pointer was provided.";
	case VDP_STATUS_INVALID_CHROMA_TYPE: return "An invalid/unsupported VdpChromaType value was supplied.";
	case VDP_STATUS_INVALID_Y_CB_CR_FORMAT: return "An invalid/unsupported VdpYCbCrFormat value was supplied.";
	case VDP_STATUS_INVALID_DECODER_PROFILE: return "An invalid/unsupported VdpDecoderProfile value was supplied.";
	case VDP_STATUS_INVALID_SIZE: return "An invalid/unsupported size was supplied.";
	case VDP_STATUS_RESOURCES: return "The system does not have enough resources to complete the requested operation.";
	case VDP_STATUS_HANDLE_DEVICE_MISMATCH: return "The handles supplied belong to different devices.";
	case VDP_STATUS_ERROR: return "A catch-all error, used when no other error code applies.";
	default: return "Unknown or unsupported status.";
	}
}

static VdpStatus vdp_get_api_version(uint32_t *api_version)
{
	if (!api_version)
		return VDP_STATUS_INVALID_POINTER;
	*api_version = 1;
	return VDP_STATUS_OK;
}

static VdpStatus vdp_get_information_string(char const **information_string)
{
	if (!information_string)
		return VDP_STATUS_INVALID_POINTER;
	*information_string = "sun4i-drm VDPAU driver (cedrus V4L2 stateless decoder)";
	return VDP_STATUS_OK;
}

static VdpStatus vdp_preemption_callback_register(VdpDevice device,
						  VdpPreemptionCallback callback, void *context)
{
	(void)callback;
	(void)context;
	return handle_get(device) ? VDP_STATUS_OK : VDP_STATUS_INVALID_HANDLE;
}

static void *const functions[] = {
	[VDP_FUNC_ID_GET_ERROR_STRING] = vdp_get_error_string,
	[VDP_FUNC_ID_GET_PROC_ADDRESS] = vdp_get_proc_address,
	[VDP_FUNC_ID_GET_API_VERSION] = vdp_get_api_version,
	[VDP_FUNC_ID_GET_INFORMATION_STRING] = vdp_get_information_string,
	[VDP_FUNC_ID_DEVICE_DESTROY] = vdp_device_destroy,
	[VDP_FUNC_ID_VIDEO_SURFACE_QUERY_CAPABILITIES] = vdp_video_surface_query_capabilities,
	[VDP_FUNC_ID_VIDEO_SURFACE_QUERY_GET_PUT_BITS_Y_CB_CR_CAPABILITIES] = vdp_video_surface_query_ycbcr_capabilities,
	[VDP_FUNC_ID_VIDEO_SURFACE_CREATE] = vdp_video_surface_create,
	[VDP_FUNC_ID_VIDEO_SURFACE_DESTROY] = vdp_video_surface_destroy,
	[VDP_FUNC_ID_VIDEO_SURFACE_GET_PARAMETERS] = vdp_video_surface_get_parameters,
	[VDP_FUNC_ID_VIDEO_SURFACE_GET_BITS_Y_CB_CR] = vdp_video_surface_get_bits_ycbcr,
	[VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR] = vdp_video_surface_put_bits_ycbcr,
	[VDP_FUNC_ID_DECODER_QUERY_CAPABILITIES] = vdp_decoder_query_capabilities,
	[VDP_FUNC_ID_DECODER_CREATE] = vdp_decoder_create,
	[VDP_FUNC_ID_DECODER_DESTROY] = vdp_decoder_destroy,
	[VDP_FUNC_ID_DECODER_GET_PARAMETERS] = vdp_decoder_get_parameters,
	[VDP_FUNC_ID_DECODER_RENDER] = vdp_decoder_render,
	[VDP_FUNC_ID_PREEMPTION_CALLBACK_REGISTER] = vdp_preemption_callback_register,
	[VDP_FUNC_ID_GENERATE_CSC_MATRIX] = vdp_generate_csc_matrix,
	[VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_CAPABILITIES] = vdp_output_surface_query_capabilities,
	[VDP_FUNC_ID_OUTPUT_SURFACE_CREATE] = vdp_output_surface_create,
	[VDP_FUNC_ID_OUTPUT_SURFACE_DESTROY] = vdp_output_surface_destroy,
	[VDP_FUNC_ID_OUTPUT_SURFACE_GET_PARAMETERS] = vdp_output_surface_get_parameters,
	[VDP_FUNC_ID_OUTPUT_SURFACE_GET_BITS_NATIVE] = vdp_output_surface_get_bits_native,
	[VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_NATIVE] = vdp_output_surface_put_bits_native,
	[VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_INDEXED] = vdp_output_surface_put_bits_indexed,
	[VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_OUTPUT_SURFACE] = vdp_output_surface_render_output_surface,
	[VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_BITMAP_SURFACE] = vdp_output_surface_render_bitmap_surface,
	[VDP_FUNC_ID_BITMAP_SURFACE_QUERY_CAPABILITIES] = vdp_bitmap_surface_query_capabilities,
	[VDP_FUNC_ID_BITMAP_SURFACE_CREATE] = vdp_bitmap_surface_create,
	[VDP_FUNC_ID_BITMAP_SURFACE_DESTROY] = vdp_bitmap_surface_destroy,
	[VDP_FUNC_ID_BITMAP_SURFACE_PUT_BITS_NATIVE] = vdp_bitmap_surface_put_bits_native,
	[VDP_FUNC_ID_VIDEO_MIXER_QUERY_FEATURE_SUPPORT] = vdp_video_mixer_query_feature_support,
	[VDP_FUNC_ID_VIDEO_MIXER_CREATE] = vdp_video_mixer_create,
	[VDP_FUNC_ID_VIDEO_MIXER_DESTROY] = vdp_video_mixer_destroy,
	[VDP_FUNC_ID_VIDEO_MIXER_SET_FEATURE_ENABLES] = vdp_video_mixer_set_feature_enables,
	[VDP_FUNC_ID_VIDEO_MIXER_SET_ATTRIBUTE_VALUES] = vdp_video_mixer_set_attribute_values,
	[VDP_FUNC_ID_VIDEO_MIXER_RENDER] = vdp_video_mixer_render,
	[VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_DESTROY] = vdp_presentation_queue_target_destroy,
	[VDP_FUNC_ID_PRESENTATION_QUEUE_CREATE] = vdp_presentation_queue_create,
	[VDP_FUNC_ID_PRESENTATION_QUEUE_DESTROY] = vdp_presentation_queue_destroy,
	[VDP_FUNC_ID_PRESENTATION_QUEUE_SET_BACKGROUND_COLOR] = vdp_presentation_queue_set_background_color,
	[VDP_FUNC_ID_PRESENTATION_QUEUE_GET_BACKGROUND_COLOR] = vdp_presentation_queue_get_background_color,
	[VDP_FUNC_ID_PRESENTATION_QUEUE_GET_TIME] = vdp_presentation_queue_get_time,
	[VDP_FUNC_ID_PRESENTATION_QUEUE_DISPLAY] = vdp_presentation_queue_display,
	[VDP_FUNC_ID_PRESENTATION_QUEUE_BLOCK_UNTIL_SURFACE_IDLE] = vdp_presentation_queue_block_until_surface_idle,
	[VDP_FUNC_ID_PRESENTATION_QUEUE_QUERY_SURFACE_STATUS] = vdp_presentation_queue_query_surface_status,
	[VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_CREATE_X11] = vdp_presentation_queue_target_create_x11,
};

VdpStatus vdp_get_proc_address(VdpDevice device, VdpFuncId function_id, void **function_pointer)
{
	if (!function_pointer)
		return VDP_STATUS_INVALID_POINTER;
	if (!handle_get(device))
		return VDP_STATUS_INVALID_HANDLE;
	if (function_id >= sizeof(functions) / sizeof(functions[0]) || !functions[function_id]) {
		DBG("unimplemented function %u\n", function_id);
		*function_pointer = NULL;
		return VDP_STATUS_INVALID_FUNC_ID;
	}
	*function_pointer = functions[function_id];
	return VDP_STATUS_OK;
}
