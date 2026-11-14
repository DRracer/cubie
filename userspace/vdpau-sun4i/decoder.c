// SPDX-License-Identifier: MIT
// Copyright (c) 2026 D.R.racer <drracer@seznam.cz>
/* VDPAU decoder on top of the cedrus V4L2 stateless (request API) decoder. */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>

#include "vdpau_private.h"

#define MAX_WIDTH	2048
#define MAX_HEIGHT	2048

static bool is_h264(VdpDecoderProfile profile)
{
	switch (profile) {
	case VDP_DECODER_PROFILE_H264_BASELINE:
	case VDP_DECODER_PROFILE_H264_CONSTRAINED_BASELINE:
	case VDP_DECODER_PROFILE_H264_MAIN:
	case VDP_DECODER_PROFILE_H264_HIGH:
		return true;
	default:
		return false;
	}
}

VdpStatus vdp_decoder_query_capabilities(VdpDevice device, VdpDecoderProfile profile,
					 VdpBool *is_supported, uint32_t *max_level,
					 uint32_t *max_macroblocks, uint32_t *max_width,
					 uint32_t *max_height)
{
	if (!is_supported || !max_level || !max_macroblocks || !max_width || !max_height)
		return VDP_STATUS_INVALID_POINTER;
	if (!handle_get(device))
		return VDP_STATUS_INVALID_HANDLE;

	*is_supported = is_h264(profile);
	*max_level = VDP_DECODER_LEVEL_H264_5_1;
	*max_width = MAX_WIDTH;
	*max_height = MAX_HEIGHT;
	*max_macroblocks = (MAX_WIDTH / 16) * (MAX_HEIGHT / 16);
	return VDP_STATUS_OK;
}

static int set_format(decoder_ctx_t *dec)
{
	int fd = dec->device->video_fd;
	struct v4l2_format out = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	struct v4l2_format cap = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

	out.fmt.pix.pixelformat = V4L2_PIX_FMT_H264_SLICE;
	out.fmt.pix.width = dec->width;
	out.fmt.pix.height = dec->height;
	out.fmt.pix.sizeimage = OUTPUT_BUFFER_SIZE;
	if (ioctl(fd, VIDIOC_S_FMT, &out)) {
		ERR("S_FMT OUTPUT: %s\n", strerror(errno));
		return -1;
	}

	cap.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12_32L32;
	cap.fmt.pix.width = dec->width;
	cap.fmt.pix.height = dec->height;
	if (ioctl(fd, VIDIOC_S_FMT, &cap) ||
	    cap.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12_32L32) {
		ERR("S_FMT CAPTURE: %s\n", strerror(errno));
		return -1;
	}
	dec->cap_width = cap.fmt.pix.width;
	dec->cap_height = cap.fmt.pix.height;
	dec->cap_stride = cap.fmt.pix.bytesperline;
	dec->cap_sizeimage = cap.fmt.pix.sizeimage;
	DBG("capture %ux%u stride %u size %u\n", dec->cap_width, dec->cap_height,
	    dec->cap_stride, dec->cap_sizeimage);
	return 0;
}

static int set_codec_mode(decoder_ctx_t *dec)
{
	struct v4l2_ext_control ctrls[] = {
		{ .id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		  .value = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED },
		{ .id = V4L2_CID_STATELESS_H264_START_CODE,
		  .value = V4L2_STATELESS_H264_START_CODE_NONE },
	};
	struct v4l2_ext_controls ext = {
		.which = V4L2_CTRL_WHICH_CUR_VAL,
		.count = 2,
		.controls = ctrls,
	};

	if (ioctl(dec->device->video_fd, VIDIOC_S_EXT_CTRLS, &ext)) {
		ERR("decode mode / start code: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int setup_output(decoder_ctx_t *dec)
{
	int fd = dec->device->video_fd;
	struct v4l2_requestbuffers rb = {
		.count = OUTPUT_BUFFERS,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_MMAP,
	};
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	if (ioctl(fd, VIDIOC_REQBUFS, &rb) || rb.count < OUTPUT_BUFFERS) {
		ERR("REQBUFS OUTPUT: %s\n", strerror(errno));
		return -1;
	}
	for (unsigned int i = 0; i < OUTPUT_BUFFERS; i++) {
		struct v4l2_buffer b = {
			.index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
			.memory = V4L2_MEMORY_MMAP,
		};

		if (ioctl(fd, VIDIOC_QUERYBUF, &b))
			return -1;
		dec->output_length[i] = b.length;
		dec->output_map[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
					  MAP_SHARED, fd, b.m.offset);
		if (dec->output_map[i] == MAP_FAILED) {
			dec->output_map[i] = NULL;
			return -1;
		}
		if (ioctl(dec->device->media_fd, MEDIA_IOC_REQUEST_ALLOC, &dec->request_fd[i])) {
			ERR("REQUEST_ALLOC: %s\n", strerror(errno));
			return -1;
		}
	}
	if (ioctl(fd, VIDIOC_STREAMON, &type)) {
		ERR("STREAMON OUTPUT: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static void teardown(decoder_ctx_t *dec)
{
	int fd = dec->device->video_fd;
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	struct v4l2_requestbuffers rb = { .count = 0, .memory = V4L2_MEMORY_MMAP };

	ioctl(fd, VIDIOC_STREAMOFF, &type);
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(fd, VIDIOC_STREAMOFF, &type);

	for (unsigned int i = 0; i < OUTPUT_BUFFERS; i++) {
		if (dec->output_map[i])
			munmap(dec->output_map[i], dec->output_length[i]);
		if (dec->request_fd[i] > 0)
			close(dec->request_fd[i]);
	}
	for (unsigned int i = 0; i < dec->capture_count; i++)
		if (dec->capture[i].map)
			munmap(dec->capture[i].map, dec->capture[i].length);

	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ioctl(fd, VIDIOC_REQBUFS, &rb);
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(fd, VIDIOC_REQBUFS, &rb);
}

VdpStatus vdp_decoder_create(VdpDevice device, VdpDecoderProfile profile, uint32_t width,
			     uint32_t height, uint32_t max_references, VdpDecoder *decoder)
{
	device_ctx_t *dev = handle_get(device);
	decoder_ctx_t *dec;

	if (!decoder)
		return VDP_STATUS_INVALID_POINTER;
	if (!dev)
		return VDP_STATUS_INVALID_HANDLE;
	if (!is_h264(profile))
		return VDP_STATUS_INVALID_DECODER_PROFILE;
	if (!width || !height || width > MAX_WIDTH || height > MAX_HEIGHT)
		return VDP_STATUS_INVALID_SIZE;
	if (dev->decoder)
		return VDP_STATUS_RESOURCES;	/* one cedrus context per device */

	dec = calloc(1, sizeof(*dec));
	if (!dec)
		return VDP_STATUS_RESOURCES;
	dec->device = dev;
	dec->profile = profile;
	dec->width = width;
	dec->height = height;
	dec->max_references = max_references;
	{
		static unsigned int generations;

		dec->generation = ++generations;
	}
	for (unsigned int i = 0; i < OUTPUT_BUFFERS; i++)
		dec->request_fd[i] = -1;
	dec->pipeline = getenv("VDPAU_SUN4I_PIPELINE") != NULL;
	pthread_mutex_init(&dec->lock, NULL);
	dec->pending_req = dec->pending_target = -1;

	if (set_format(dec) || set_codec_mode(dec) || setup_output(dec) || h264_init(dec)) {
		teardown(dec);
		h264_free(dec);
		free(dec);
		return VDP_STATUS_ERROR;
	}

	*decoder = handle_create(dec);
	if (*decoder == VDP_INVALID_HANDLE) {
		teardown(dec);
		h264_free(dec);
		free(dec);
		return VDP_STATUS_RESOURCES;
	}
	dev->decoder = dec;
	DBG("decoder %ux%u profile %u max_references %u\n", width, height, profile, max_references);
	return VDP_STATUS_OK;
}

static void complete_pending(decoder_ctx_t *dec);

VdpStatus vdp_decoder_destroy(VdpDecoder decoder)
{
	decoder_ctx_t *dec = handle_get(decoder);
	device_ctx_t *dev;

	if (!dec)
		return VDP_STATUS_INVALID_HANDLE;
	dev = dec->device;
	/* no sync caller or render may use the decoder from here on */
	pthread_mutex_lock(&dev->decoder_lock);
	pthread_mutex_lock(&dec->lock);
	complete_pending(dec);
	pthread_mutex_unlock(&dec->lock);
	/* the presentation worker must not show pictures of this decoder anymore */
	pthread_mutex_lock(&dev->drm_lock);
	if (dev->drm.fd >= 0)
		drm_release_video_fbs(dev, dec);
	pthread_mutex_lock(&dev->ref_lock);
	dev->decoder = NULL;
	pthread_mutex_unlock(&dev->ref_lock);
	pthread_mutex_unlock(&dev->drm_lock);
	pthread_mutex_unlock(&dev->decoder_lock);
	teardown(dec);
	h264_free(dec);
	pthread_mutex_destroy(&dec->lock);
	handle_destroy(decoder);
	free(dec);
	return VDP_STATUS_OK;
}

VdpStatus vdp_decoder_get_parameters(VdpDecoder decoder, VdpDecoderProfile *profile,
				     uint32_t *width, uint32_t *height)
{
	decoder_ctx_t *dec = handle_get(decoder);

	if (!profile || !width || !height)
		return VDP_STATUS_INVALID_POINTER;
	if (!dec)
		return VDP_STATUS_INVALID_HANDLE;
	*profile = dec->profile;
	*width = dec->width;
	*height = dec->height;
	return VDP_STATUS_OK;
}

/* Give the surface a CAPTURE buffer: reuse a released one or create one. */
int decoder_bind_surface(decoder_ctx_t *dec, video_surface_ctx_t *surface)
{
	int fd = dec->device->video_fd;
	struct v4l2_create_buffers create = {
		.count = 1,
		.memory = V4L2_MEMORY_MMAP,
		.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		/*
		 * The CPU reads pictures back (GetBitsYCbCr); vb2 syncs caches. Needs
		 * the cedrus-dec cache-hint patch; ignored (uncached) on stock kernels.
		 */
		.flags = V4L2_MEMORY_FLAG_NON_COHERENT,
	};
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	struct capture_buffer *cb;
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	unsigned int i;

	/* An index from an earlier, destroyed decoder is meaningless here. */
	if (surface->index >= 0 && surface->generation == dec->generation)
		return 0;
	surface->index = -1;
	surface->generation = dec->generation;

	for (i = 0; i < dec->capture_count; i++) {
		if (!dec->capture[i].refs) {
			dec->capture[i].refs = 1;
			dec->capture[i].decoded = false;
			surface->index = i;
			return 0;
		}
	}
	if (dec->capture_count >= MAX_SURFACES)
		return -1;

	if (ioctl(fd, VIDIOC_G_FMT, &create.format) ||
	    ioctl(fd, VIDIOC_CREATE_BUFS, &create) || create.count != 1) {
		ERR("CREATE_BUFS CAPTURE: %s\n", strerror(errno));
		return -1;
	}
	i = create.index;
	if (i >= MAX_SURFACES || i != dec->capture_count) {
		ERR("unexpected CAPTURE buffer index %u\n", i);
		return -1;
	}
	b.index = i;
	if (ioctl(fd, VIDIOC_QUERYBUF, &b))
		return -1;

	cb = &dec->capture[i];
	cb->length = b.length;
	cb->map = mmap(NULL, b.length, PROT_READ, MAP_SHARED, fd, b.m.offset);
	if (cb->map == MAP_FAILED) {
		cb->map = NULL;
		return -1;
	}
	/* reference_ts in ns; the OUTPUT timestamp (tv_usec) is copied to it. */
	cb->timestamp = (uint64_t)(i + 1) * 1000;
	cb->refs = 1;
	cb->decoded = false;
	dec->capture_count++;

	if (dec->capture_count == 1 && ioctl(fd, VIDIOC_STREAMON, &type)) {
		ERR("STREAMON CAPTURE: %s\n", strerror(errno));
		return -1;
	}
	surface->index = i;
	DBG("surface %p -> capture buffer %u (%zu bytes)\n", (void *)surface, i, cb->length);
	return 0;
}

void decoder_release_surface(decoder_ctx_t *dec, video_surface_ctx_t *surface)
{
	if (surface->index >= 0 && surface->generation == dec->generation &&
	    (unsigned int)surface->index < dec->capture_count &&
	    dec->capture[surface->index].refs > 0)
		dec->capture[surface->index].refs--;
	surface->index = -1;
}

int decoder_surface_picture(decoder_ctx_t *dec, video_surface_ctx_t *surface)
{
	if (!dec || surface->index < 0 || surface->generation != dec->generation ||
	    (unsigned int)surface->index >= dec->capture_count ||
	    (!dec->capture[surface->index].decoded && surface->index != dec->pending_target))
		return -1;
	return surface->index;
}

void decoder_ref_buffer(decoder_ctx_t *dec, int index)
{
	dec->capture[index].refs++;
}

void decoder_unref_buffer(device_ctx_t *dev, int index, unsigned int gen)
{
	decoder_ctx_t *dec = dev->decoder;

	if (index < 0 || !dec || dec->generation != gen || (unsigned int)index >= dec->capture_count)
		return;
	if (dec->capture[index].refs > 0)
		dec->capture[index].refs--;
}

/*
 * Dequeue finished CAPTURE buffers we are not waiting for: a picture whose
 * slices failed half-way leaves its (held) buffer to be completed by the
 * kernel later. Returns 0 if `want` was dequeued without error flag.
 */
static int drain_capture(decoder_ctx_t *dec, int want)
{
	int ret = -1;

	for (;;) {
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
		};

		if (ioctl(dec->device->video_fd, VIDIOC_DQBUF, &b))
			return ret;	/* EAGAIN: nothing more done */
		if ((int)b.index == want)
			ret = (b.flags & V4L2_BUF_FLAG_ERROR) ? -1 : 0;
		/* only the pipelined picture completes here; a stale buffer of a
		 * picture that failed half-way stays not decoded */
		if ((int)b.index == dec->pending_target)
			dec->capture[b.index].decoded = !(b.flags & V4L2_BUF_FLAG_ERROR);
		else if ((int)b.index != want)
			DBG("drained stale CAPTURE buffer %u\n", b.index);
	}
}

static int wait_request(int request_fd)
{
	struct pollfd p = { .fd = request_fd, .events = POLLPRI };
	int ret = poll(&p, 1, 2000);

	if (ret <= 0) {
		ERR("request %s\n", ret ? strerror(errno) : "timed out");
		return -1;
	}
	if (!(p.revents & POLLPRI)) {
		ERR("request poll: revents %#x\n", p.revents);
		return -1;
	}
	return 0;
}

static void complete_pending(decoder_ctx_t *dec)
{
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP };
	int req;

	if (dec->pending_req < 0)
		return;
	req = dec->request_fd[dec->pending_req];
	if (!wait_request(req))
		/* with two in flight, any finished OUTPUT buffer will do */
		while (!ioctl(dec->device->video_fd, VIDIOC_DQBUF, &b))
			b = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
						  .memory = V4L2_MEMORY_MMAP };
	drain_capture(dec, dec->pending_target);	/* sets its decoded flag */
	if (ioctl(req, MEDIA_REQUEST_IOC_REINIT, NULL))
		ERR("REQUEST_REINIT: %s\n", strerror(errno));
	dec->pending_req = dec->pending_target = -1;
}

void decoder_sync(device_ctx_t *dev)
{
	decoder_ctx_t *dec = dev->decoder;

	if (!dec)
		return;
	pthread_mutex_lock(&dec->lock);
	complete_pending(dec);
	pthread_mutex_unlock(&dec->lock);
}

int decoder_submit_slice(decoder_ctx_t *dec, video_surface_ctx_t *target,
			 const struct v4l2_ext_control *ctrls, unsigned int num_ctrls,
			 const struct v4l2_slice_job *job)
{
	int fd = dec->device->video_fd;
	unsigned int i = dec->output_next;
	int req = dec->request_fd[i];
	struct capture_buffer *cb = &dec->capture[target->index];
	struct v4l2_ext_controls ext = {
		.which = V4L2_CTRL_WHICH_REQUEST_VAL,
		.count = num_ctrls,
		.request_fd = req,
		.controls = (struct v4l2_ext_control *)ctrls,
	};
	struct v4l2_buffer out = {
		.index = i,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_MMAP,
		.bytesused = job->size,
		.flags = V4L2_BUF_FLAG_REQUEST_FD |
			 (job->last_slice ? 0 : V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF),
		.request_fd = req,
		.timestamp = { .tv_sec = 0, .tv_usec = target->index + 1 },
	};
	struct v4l2_buffer b;
	int ret = -1;

	if (job->first_slice && !(job->last_slice && dec->pipeline))
		complete_pending(dec);	/* multi-slice pictures run synchronously */
	if (job->first_slice)
		drain_capture(dec, -1);
	if ((int)i == dec->pending_req)
		complete_pending(dec);	/* its OUTPUT buffer is about to be reused */
	if (job->size > dec->output_length[i]) {
		ERR("slice of %zu bytes exceeds the OUTPUT buffer\n", job->size);
		return -1;
	}
	dec->output_next = (i + 1) % OUTPUT_BUFFERS;
	memcpy(dec->output_map[i], job->data, job->size);

	if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ext)) {
		ERR("S_EXT_CTRLS (request): %s (error_idx %u)\n", strerror(errno), ext.error_idx);
		goto reinit;
	}

	if (job->first_slice) {
		b = (struct v4l2_buffer){
			.index = target->index,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
		};
		if (ioctl(fd, VIDIOC_QBUF, &b)) {
			ERR("QBUF CAPTURE %d: %s\n", target->index, strerror(errno));
			goto reinit;
		}
	}

	if (ioctl(fd, VIDIOC_QBUF, &out)) {
		ERR("QBUF OUTPUT: %s\n", strerror(errno));
		goto reinit;
	}
	if (ioctl(req, MEDIA_REQUEST_IOC_QUEUE, NULL)) {
		ERR("REQUEST_QUEUE: %s\n", strerror(errno));
		goto reinit;
	}
	/* pipeline: leave a single-slice picture in flight, finish the previous one */
	if (dec->pipeline && job->first_slice && job->last_slice) {
		complete_pending(dec);
		dec->pending_req = i;
		dec->pending_target = target->index;
		return 0;
	}
	if (wait_request(req))
		goto reinit;

	b = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP };
	if (ioctl(fd, VIDIOC_DQBUF, &b)) {
		ERR("DQBUF OUTPUT: %s\n", strerror(errno));
		goto reinit;
	}
	ret = (b.flags & V4L2_BUF_FLAG_ERROR) ? -1 : 0;

	/* dst completes before src (v4l2-mem2mem), so it is ready now */
	if (job->last_slice) {
		if (drain_capture(dec, target->index))
			ret = -1;
		dec->capture[target->index].decoded = ret == 0;
	} else {
		drain_capture(dec, -1);
	}
	(void)cb;

reinit:
	if (ioctl(req, MEDIA_REQUEST_IOC_REINIT, NULL))
		ERR("REQUEST_REINIT: %s\n", strerror(errno));
	return ret;
}

VdpStatus vdp_decoder_render(VdpDecoder decoder, VdpVideoSurface target,
			     VdpPictureInfo const *picture_info, uint32_t bitstream_buffer_count,
			     VdpBitstreamBuffer const *bitstream_buffers)
{
	decoder_ctx_t *dec = handle_get(decoder);
	video_surface_ctx_t *surface = handle_get(target);

	if (!picture_info || !bitstream_buffers)
		return VDP_STATUS_INVALID_POINTER;
	if (!dec || !surface)
		return VDP_STATUS_INVALID_HANDLE;
	if (surface->device != dec->device)
		return VDP_STATUS_HANDLE_DEVICE_MISMATCH;
	/* still shown (output surface or plane): decode into a fresh buffer */
	pthread_mutex_lock(&dec->device->ref_lock);
	if (surface->index >= 0 && surface->generation == dec->generation &&
	    dec->capture[surface->index].refs > 1)
		decoder_release_surface(dec, surface);
	if (decoder_bind_surface(dec, surface)) {
		pthread_mutex_unlock(&dec->device->ref_lock);
		ERR("out of CAPTURE buffers (%u in use)\n", dec->capture_count);
		return VDP_STATUS_RESOURCES;
	}
	pthread_mutex_unlock(&dec->device->ref_lock);

	{
		VdpStatus st;

		pthread_mutex_lock(&dec->lock);
		st = h264_decode(dec, surface, (const VdpPictureInfoH264 *)picture_info,
				 bitstream_buffer_count, bitstream_buffers);
		pthread_mutex_unlock(&dec->lock);
		return st;
	}
}
