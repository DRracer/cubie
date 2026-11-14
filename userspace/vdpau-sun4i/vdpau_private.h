// SPDX-License-Identifier: MIT
// Copyright (c) 2026 D.R.racer <drracer@seznam.cz>
/*
 * libvdpau_sun4i-drm — VDPAU driver for the Allwinner A20 (cedrus V4L2
 * stateless decoder, sun4i-drm display).
 *
 * H.264 decode (progressive frame pictures) with YCbCr read-back, and a
 * presentation path for `mpv --vo=vdpau`: the video mixer only records which
 * video surface goes where; output surfaces hold the OSD (straight-alpha ARGB
 * dumb buffers); the presentation queue shows the decoded picture on one
 * overlay plane (frontend-scaled, zero-copy) and the OSD on a higher one.
 */
#ifndef VDPAU_PRIVATE_H
#define VDPAU_PRIVATE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <linux/videodev2.h>
#include <vdpau/vdpau.h>
#include <vdpau/vdpau_x11.h>
#include <X11/Xlib.h>

#define MAX_SURFACES		32	/* vb2 default maximum per queue */
#define OUTPUT_BUFFERS		2
#define OUTPUT_BUFFER_SIZE	(2 * 1024 * 1024)

extern int vdpau_debug;
#define DBG(...)	do { if (vdpau_debug) fprintf(stderr, "vdpau-sun4i: " __VA_ARGS__); } while (0)
#define ERR(...)	fprintf(stderr, "vdpau-sun4i: " __VA_ARGS__)

typedef struct device_ctx device_ctx_t;
typedef struct decoder_ctx decoder_ctx_t;

/*
 * One cedrus CAPTURE buffer. Referenced by the video surface it is bound to
 * and by every output surface showing it (the mixer does not copy pixels),
 * so decoding into a surface whose picture is still on its way to the
 * screen moves the surface to a fresh buffer instead of overwriting it.
 */
struct capture_buffer {
	int refs;		/* 0 = free */
	bool decoded;		/* holds a complete decoded picture */
	void *map;
	size_t length;
	uint64_t timestamp;	/* reference_ts identifying this buffer */
	uint32_t fb_id;		/* DRM framebuffer (presentation), 0 = none yet */
	uint32_t gem_handle;
};

struct dumb_buffer {
	uint32_t handle, fb_id, pitch;
	uint64_t size;
	void *map;
};

#define MAX_CRTCS 4

/* One display pipeline; x/y = where its scanout sits in the X screen. */
struct drm_crtc_info {
	uint32_t id;
	int index;
	bool active;
	int x, y;
	uint32_t w, h;
	uint32_t video_plane, osd_plane;
};

struct drm_state {
	int fd;			/* X-authenticated card0, -1 = not opened */
	struct drm_crtc_info crtcs[MAX_CRTCS];
	int ncrtcs;
};

struct device_ctx {
	Display *display;
	int screen;
	int video_fd;		/* cedrus decoder node */
	int media_fd;		/* its media controller (requests) */
	decoder_ctx_t *decoder;	/* the one active decoder, if any */
	struct drm_state drm;
	pthread_mutex_t drm_lock; /* plane updates vs framebuffer teardown */
	/* CAPTURE buffer refs/binding: app thread and presentation worker. Held
	 * only briefly, never across a blocking plane update (drm_lock -> ref_lock). */
	pthread_mutex_t ref_lock;
	/* lifetime of `decoder` vs. decoder_sync() callers (mixer, GetBits):
	 * decoder_lock -> decoder->lock -> drm_lock -> ref_lock */
	pthread_mutex_t decoder_lock;
	void *queue;		/* the presentation queue of this device, if any */
};

typedef struct video_surface_ctx {
	device_ctx_t *device;
	VdpChromaType chroma_type;
	uint32_t width, height;
	int index;		/* CAPTURE buffer index, -1 = not bound yet */
	unsigned int generation; /* decoder the index belongs to */
} video_surface_ctx_t;

struct decoder_ctx {
	device_ctx_t *device;
	VdpDecoderProfile profile;
	uint32_t width, height;	/* coded size given to VdpDecoderCreate */
	uint32_t max_references;

	/* CAPTURE format as set by the driver */
	uint32_t cap_width, cap_height, cap_stride, cap_sizeimage;
	struct capture_buffer capture[MAX_SURFACES];
	unsigned int capture_count;

	void *output_map[OUTPUT_BUFFERS];
	size_t output_length[OUTPUT_BUFFERS];
	unsigned int output_next;
	int request_fd[OUTPUT_BUFFERS];

	unsigned int generation; /* distinguishes successive decoders */
	/* render vs. sync from other threads (FFmpeg 7 runs hwdownload in the
	 * filter thread, mpv mixes in the VO thread) */
	pthread_mutex_t lock;

	/* EXPERIMENT: keep one picture's request in flight while queueing
	 * the next (as GStreamer does), VDPAU_SUN4I_PIPELINE=1. Known limits:
	 * a failed wait leaves the request queued, an error of the in-flight
	 * picture is not reported to the picture that references it, and a
	 * surface released while in flight is not waited for. */
	bool pipeline;
	int pending_req;	/* OUTPUT/request index in flight, -1 = none */
	int pending_target;	/* its CAPTURE buffer */

	void *codec_priv;	/* h264_ctx_t */
};

typedef struct {
	VdpRGBAFormat format;
	uint32_t width, height;
	uint8_t *data;		/* malloc'ed, width * height * bpp */
	unsigned int bpp;
} bitmap_surface_ctx_t;

typedef struct output_surface_ctx {
	device_ctx_t *device;
	VdpRGBAFormat format;
	uint32_t width, height;

	/* picture placed by the mixer (shown on the video plane): a referenced
	 * CAPTURE buffer of decoder `video_gen`, -1 = none */
	int video_buf;
	unsigned int video_gen;
	VdpRect video_src, video_dst;

	/* OSD: straight-alpha ARGB, allocated on first drawing. Drawing works on a
	 * cached shadow copy; changed rectangles are written to the (uncached,
	 * scanned-out) dumb buffer, which is never read back. */
	struct dumb_buffer osd;
	uint32_t *shadow;
	bool osd_used;		/* holds something non-transparent */
	bool osd_clear;		/* clear before the next drawing */
	VdpRect osd_dirty;	/* drawn area since the last clear (empty: x0 >= x1) */

	/* presentation queue bookkeeping (queue lock) */
	unsigned int pending;	/* queued jobs referring to this surface */
	VdpPresentationQueueStatus status;
	VdpTime first_presentation_time;
} output_surface_ctx_t;

typedef struct {
	device_ctx_t *device;
	uint32_t width, height;
} video_mixer_ctx_t;

typedef struct {
	device_ctx_t *device;
	Drawable drawable;
} queue_target_ctx_t;

/* handles.c */
uint32_t handle_create(void *data);
void *handle_get(uint32_t handle);
void handle_destroy(uint32_t handle);

/* device.c */
VdpStatus vdp_get_proc_address(VdpDevice device, VdpFuncId function_id, void **function_pointer);

/* surface_video.c */
VdpVideoSurfaceCreate vdp_video_surface_create;
VdpVideoSurfaceDestroy vdp_video_surface_destroy;
VdpVideoSurfaceGetParameters vdp_video_surface_get_parameters;
VdpVideoSurfaceGetBitsYCbCr vdp_video_surface_get_bits_ycbcr;
VdpVideoSurfacePutBitsYCbCr vdp_video_surface_put_bits_ycbcr;
VdpVideoSurfaceQueryCapabilities vdp_video_surface_query_capabilities;
VdpVideoSurfaceQueryGetPutBitsYCbCrCapabilities vdp_video_surface_query_ycbcr_capabilities;

/* decoder.c */
VdpDecoderCreate vdp_decoder_create;
VdpDecoderDestroy vdp_decoder_destroy;
VdpDecoderGetParameters vdp_decoder_get_parameters;
VdpDecoderQueryCapabilities vdp_decoder_query_capabilities;
VdpDecoderRender vdp_decoder_render;
int decoder_bind_surface(decoder_ctx_t *dec, video_surface_ctx_t *surface);
void decoder_release_surface(decoder_ctx_t *dec, video_surface_ctx_t *surface);
/* surface's picture: CAPTURE index if bound to `dec` and decoded, else -1 */
int decoder_surface_picture(decoder_ctx_t *dec, video_surface_ctx_t *surface);
void decoder_ref_buffer(decoder_ctx_t *dec, int index);
/* finish a picture of the device's decoder still in flight (pipeline mode);
 * caller holds dev->decoder_lock */
void decoder_sync(device_ctx_t *dev);
/* drops a reference taken under decoder generation `gen` (no-op if stale) */
void decoder_unref_buffer(device_ctx_t *dev, int index, unsigned int gen);

/* One slice handed to the V4L2 layer. */
struct v4l2_slice_job {
	const uint8_t *data;	/* NAL unit without start code */
	size_t size;
	bool last_slice;	/* release the CAPTURE buffer afterwards */
	bool first_slice;	/* queue the CAPTURE buffer first */
};

int decoder_submit_slice(decoder_ctx_t *dec, video_surface_ctx_t *target,
			 const struct v4l2_ext_control *ctrls, unsigned int num_ctrls,
			 const struct v4l2_slice_job *job);

/* drm.c */
int drm_open(device_ctx_t *dev);
void drm_close(device_ctx_t *dev);
uint32_t drm_video_fb(device_ctx_t *dev, decoder_ctx_t *dec, unsigned int index);
void drm_release_video_fbs(device_ctx_t *dev, decoder_ctx_t *dec);
int drm_dumb_create(device_ctx_t *dev, uint32_t width, uint32_t height, struct dumb_buffer *b);
void drm_dumb_destroy(device_ctx_t *dev, struct dumb_buffer *b);
int drm_show(device_ctx_t *dev, uint32_t crtc, uint32_t plane, uint32_t fb, int x, int y, unsigned int w,
	     unsigned int h, unsigned int sx, unsigned int sy, unsigned int sw, unsigned int sh);
/* refresh all CRTCs' modes and positions (xrandr); returns 0 if any is active */
int drm_update_crtcs(device_ctx_t *dev);
/* index (into drm.crtcs) of the active CRTC covering most of the rectangle,
 * `cur` on a tie; -1 if none is active */
int drm_pick_crtc(device_ctx_t *dev, int cur, int x, int y, uint32_t w, uint32_t h);
/* time of the most recent vblank of our CRTC (CLOCK_MONOTONIC ns), 0 if unknown */
VdpTime drm_last_vblank(device_ctx_t *dev, int crtc);

/* surface_output.c */
VdpOutputSurfaceQueryCapabilities vdp_output_surface_query_capabilities;
VdpOutputSurfaceCreate vdp_output_surface_create;
VdpOutputSurfaceDestroy vdp_output_surface_destroy;
VdpOutputSurfaceGetParameters vdp_output_surface_get_parameters;
VdpOutputSurfaceGetBitsNative vdp_output_surface_get_bits_native;
VdpOutputSurfacePutBitsNative vdp_output_surface_put_bits_native;
VdpOutputSurfacePutBitsIndexed vdp_output_surface_put_bits_indexed;
VdpOutputSurfaceRenderOutputSurface vdp_output_surface_render_output_surface;
VdpOutputSurfaceRenderBitmapSurface vdp_output_surface_render_bitmap_surface;
VdpBitmapSurfaceQueryCapabilities vdp_bitmap_surface_query_capabilities;
VdpBitmapSurfaceCreate vdp_bitmap_surface_create;
VdpBitmapSurfaceDestroy vdp_bitmap_surface_destroy;
VdpBitmapSurfacePutBitsNative vdp_bitmap_surface_put_bits_native;

/* mixer.c */
VdpVideoMixerQueryFeatureSupport vdp_video_mixer_query_feature_support;
VdpVideoMixerCreate vdp_video_mixer_create;
VdpVideoMixerDestroy vdp_video_mixer_destroy;
VdpVideoMixerSetFeatureEnables vdp_video_mixer_set_feature_enables;
VdpVideoMixerSetAttributeValues vdp_video_mixer_set_attribute_values;
VdpVideoMixerRender vdp_video_mixer_render;
VdpGenerateCSCMatrix vdp_generate_csc_matrix;

/* presentation_queue.c */
VdpPresentationQueueTargetCreateX11 vdp_presentation_queue_target_create_x11;
VdpPresentationQueueTargetDestroy vdp_presentation_queue_target_destroy;
VdpPresentationQueueCreate vdp_presentation_queue_create;
VdpPresentationQueueDestroy vdp_presentation_queue_destroy;
VdpPresentationQueueSetBackgroundColor vdp_presentation_queue_set_background_color;
VdpPresentationQueueGetBackgroundColor vdp_presentation_queue_get_background_color;
VdpPresentationQueueGetTime vdp_presentation_queue_get_time;
VdpPresentationQueueDisplay vdp_presentation_queue_display;
VdpPresentationQueueBlockUntilSurfaceIdle vdp_presentation_queue_block_until_surface_idle;
VdpPresentationQueueQuerySurfaceStatus vdp_presentation_queue_query_surface_status;
/* An output surface's video or OSD content is going away: stop showing it. */
void presentation_forget_surface(device_ctx_t *dev, output_surface_ctx_t *os);

/* h264.c */
int h264_init(decoder_ctx_t *dec);
void h264_free(decoder_ctx_t *dec);
VdpStatus h264_decode(decoder_ctx_t *dec, video_surface_ctx_t *target,
		      const VdpPictureInfoH264 *info,
		      uint32_t buffer_count, const VdpBitstreamBuffer *buffers);

#endif
