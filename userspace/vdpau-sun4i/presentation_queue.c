// SPDX-License-Identifier: MIT
// Copyright (c) 2026 D.R.racer <drracer@seznam.cz>
/*
 * Presentation queue: Display() queues an output surface and returns; a
 * worker thread waits for the requested time, places the decoded picture on
 * the video overlay plane over the target window (the display frontend
 * scales it) and the surface's OSD on the OSD plane, then reports the
 * surface VISIBLE (at the vblank that showed it) and the previous one IDLE.
 * Planes sit above X (no colour key yet): other windows overlapping the
 * video are hidden under it.
 *
 * The worker uses its own X connection, through XCB with checked replies, so
 * it neither shares the application's Display between threads nor triggers
 * Xlib's process-global error handler when the window goes away. It holds
 * its own reference on the CAPTURE buffer it shows, so the picture on the
 * screen can never be decoded into, whatever the application does.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>

#include "vdpau_private.h"

#define MAX_JOBS	16

struct job {
	output_surface_ctx_t *os;	/* NULL: cancelled */
	uint32_t clip_w, clip_h;
	VdpTime earliest;
};

typedef struct {
	device_ctx_t *device;
	queue_target_ctx_t *target;
	Display *xdpy;		/* worker's own connection (XCB used) */

	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;	/* job queued/done, surface state changed, quit */
	struct job jobs[MAX_JOBS];
	unsigned int head, count;
	output_surface_ctx_t *busy;	/* job being shown right now */
	bool quit;

	output_surface_ctx_t *visible;
	VdpColor background;

	/* worker only (under drm_lock) */
	bool video_on[MAX_CRTCS], osd_on[MAX_CRTCS];	/* per drm.crtcs[] */
	int crtc;		/* drm.crtcs[] index with most of the window, -1 = none */
	int shown_buf;		/* CAPTURE buffer on the video plane, ref held */
	unsigned int shown_gen;
	bool warned_downscale;
} queue_ctx_t;

static VdpTime now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (VdpTime)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

VdpStatus vdp_presentation_queue_target_create_x11(VdpDevice device, Drawable drawable,
						   VdpPresentationQueueTarget *target)
{
	device_ctx_t *dev = handle_get(device);
	queue_target_ctx_t *t;

	if (!target)
		return VDP_STATUS_INVALID_POINTER;
	if (!dev)
		return VDP_STATUS_INVALID_HANDLE;
	t = calloc(1, sizeof(*t));
	if (!t)
		return VDP_STATUS_RESOURCES;
	t->device = dev;
	t->drawable = drawable;
	*target = handle_create(t);
	if (*target == VDP_INVALID_HANDLE) {
		free(t);
		return VDP_STATUS_RESOURCES;
	}
	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_target_destroy(VdpPresentationQueueTarget target)
{
	queue_target_ctx_t *t = handle_get(target);

	if (!t)
		return VDP_STATUS_INVALID_HANDLE;
	handle_destroy(target);
	free(t);
	return VDP_STATUS_OK;
}

/*
 * Clip a destination rectangle to bounds [bx0,bx1)x[by0,by1), cropping the
 * source in proportion. For YUV (`even`), source offsets and sizes are kept
 * even (4:2:0 chroma; the tiled frontend path addresses CbCr pairs); an RGB
 * layer must stay exactly 1:1 instead (it has no scaler).
 */
static bool clip(int *x, int *y, int *w, int *h, unsigned int *sx, unsigned int *sy,
		 unsigned int *sw, unsigned int *sh, int bx0, int by0, int bx1, int by1, bool even)
{
	int64_t cut;

	if (*w <= 0 || *h <= 0 || !*sw || !*sh)
		return false;
	if (*x < bx0) {
		cut = (int64_t)(bx0 - *x) * *sw / *w;
		*sx += cut;
		*sw = cut < *sw ? *sw - cut : 0;
		*w -= bx0 - *x;
		*x = bx0;
	}
	if (*y < by0) {
		cut = (int64_t)(by0 - *y) * *sh / *h;
		*sy += cut;
		*sh = cut < *sh ? *sh - cut : 0;
		*h -= by0 - *y;
		*y = by0;
	}
	if (*w > 0 && *x + *w > bx1) {
		cut = (int64_t)(*x + *w - bx1) * *sw / *w;
		*sw = cut < *sw ? *sw - cut : 0;
		*w = bx1 - *x;
	}
	if (*h > 0 && *y + *h > by1) {
		cut = (int64_t)(*y + *h - by1) * *sh / *h;
		*sh = cut < *sh ? *sh - cut : 0;
		*h = by1 - *y;
	}
	if (even) {
		*sw += *sx & 1;	/* keep the right/bottom edge when rounding down */
		*sh += *sy & 1;
		*sx &= ~1U;
		*sy &= ~1U;
		*sw &= ~1U;
		*sh &= ~1U;
	}
	return *w > 0 && *h > 0 && *sw > 0 && *sh > 0;
}

/* Window position on the root window; false if gone or not viewable. */
static bool window_geometry(queue_ctx_t *q, int *win_x, int *win_y)
{
	xcb_connection_t *c = XGetXCBConnection(q->xdpy);
	xcb_window_t w = q->target->drawable;
	xcb_get_window_attributes_reply_t *a;
	xcb_translate_coordinates_reply_t *t;
	bool ok;

	a = xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, w), NULL);
	t = xcb_translate_coordinates_reply(c, xcb_translate_coordinates(c, w,
			xcb_setup_roots_iterator(xcb_get_setup(c)).data->root, 0, 0), NULL);
	ok = a && t && a->map_state == XCB_MAP_STATE_VIEWABLE;
	if (ok) {
		*win_x = t->dst_x;
		*win_y = t->dst_y;
	}
	free(a);
	free(t);
	return ok;
}

static void plane_off(device_ctx_t *dev, uint32_t plane, bool *on)
{
	if (*on && !drm_show(dev, 0, plane, 0, 0, 0, 0, 0, 0, 0, 0, 0))
		*on = false;
}

/* Both planes off on every CRTC; true if the video plane is off everywhere. */
static bool planes_off(queue_ctx_t *q)
{
	device_ctx_t *dev = q->device;
	bool all_off = true;

	for (int i = 0; i < dev->drm.ncrtcs; i++) {
		plane_off(dev, dev->drm.crtcs[i].video_plane, &q->video_on[i]);
		plane_off(dev, dev->drm.crtcs[i].osd_plane, &q->osd_on[i]);
		all_off &= !q->video_on[i];
	}
	return all_off;
}

/* Swap the worker's reference to the buffer now on the video plane. */
static void set_shown(queue_ctx_t *q, int buf, unsigned int gen)
{
	device_ctx_t *dev = q->device;

	if (buf == q->shown_buf && gen == q->shown_gen)
		return;
	pthread_mutex_lock(&dev->ref_lock);
	if (buf >= 0 && dev->decoder && dev->decoder->generation == gen)
		decoder_ref_buffer(dev->decoder, buf);
	decoder_unref_buffer(dev, q->shown_buf, q->shown_gen);
	pthread_mutex_unlock(&dev->ref_lock);
	q->shown_buf = buf;
	q->shown_gen = gen;
}

/*
 * Called with drm_lock held. A window spanning two monitors is shown on both:
 * each CRTC has its own planes and frontend, all reading the same picture.
 */
static void show(queue_ctx_t *q, output_surface_ctx_t *os, uint32_t clip_w, uint32_t clip_h,
		 int win_x, int win_y, bool viewable)
{
	device_ctx_t *dev = q->device;
	decoder_ctx_t *dec = dev->decoder;
	int buf = os->video_buf;
	uint32_t fb = 0;
	bool shown = false, any_on = false;

	if (drm_update_crtcs(dev))
		viewable = false;	/* all CRTCs off (DPMS): nothing to place */
	/* vblank timing follows the monitor with most of the window */
	if (viewable)
		q->crtc = drm_pick_crtc(dev, q->crtc, win_x, win_y, clip_w, clip_h);

	if (viewable && dec && buf >= 0 && os->video_gen == dec->generation &&
	    (unsigned int)buf < dec->capture_count && dec->capture[buf].decoded)
		fb = drm_video_fb(dev, dec, buf);

	for (int i = 0; i < dev->drm.ncrtcs; i++) {
		const struct drm_crtc_info *c = &dev->drm.crtcs[i];
		/* window and its visible part, in this CRTC's coordinates */
		int cx = win_x - c->x, cy = win_y - c->y;
		int wx1 = cx + (int)clip_w, wy1 = cy + (int)clip_h;
		bool on_crtc = viewable && c->active && cx < (int)c->w && cy < (int)c->h &&
			       wx1 > 0 && wy1 > 0;
		bool video = false, osd = false;

		if (on_crtc && fb && c->video_plane) {
			int x = cx + (int)os->video_dst.x0, y = cy + (int)os->video_dst.y0;
			int w = (int)os->video_dst.x1 - (int)os->video_dst.x0;
			int h = (int)os->video_dst.y1 - (int)os->video_dst.y0;
			unsigned int sx = os->video_src.x0, sy = os->video_src.y0;
			unsigned int sw = os->video_src.x1 - os->video_src.x0;
			unsigned int sh = os->video_src.y1 - os->video_src.y0;

			if (clip(&x, &y, &w, &h, &sx, &sy, &sw, &sh,
				 cx > 0 ? cx : 0, cy > 0 ? cy : 0,
				 wx1 < (int)c->w ? wx1 : (int)c->w,
				 wy1 < (int)c->h ? wy1 : (int)c->h, true)) {
				/* the frontend cannot fetch > ~2 source lines per output line at 1080p60 */
				if (sh > 2 * (unsigned int)h && !q->warned_downscale) {
					ERR("video scaled down %u:%d vertically: the display frontend may "
					    "disturb the desktop beside the window\n", sh, h);
					q->warned_downscale = true;
				}
				if (!drm_show(dev, c->id, c->video_plane, fb, x, y, w, h,
					      sx, sy, sw, sh))
					video = true;
				else
					DBG("video plane: %s\n", strerror(errno));
			}
		}
		if (video)
			q->video_on[i] = true;
		else
			plane_off(dev, c->video_plane, &q->video_on[i]);
		shown |= video;
		any_on |= q->video_on[i];

		if (on_crtc && os->osd_used && !os->osd_clear && os->osd.fb_id && c->osd_plane) {
			int x = cx, y = cy, w = clip_w, h = clip_h;
			unsigned int sx = 0, sy = 0, sw = clip_w, sh = clip_h;

			if (sw > os->width)
				sw = w = os->width;
			if (sh > os->height)
				sh = h = os->height;
			/* only one layer may use the frontend scaler: the OSD stays 1:1 */
			if (clip(&x, &y, &w, &h, &sx, &sy, &sw, &sh, 0, 0, c->w, c->h, false) &&
			    !drm_show(dev, c->id, c->osd_plane, os->osd.fb_id, x, y, w, h,
				      sx, sy, sw, sh))
				osd = true;
		}
		if (osd)
			q->osd_on[i] = true;
		else
			plane_off(dev, c->osd_plane, &q->osd_on[i]);
	}

	if (shown)
		set_shown(q, buf, os->video_gen);
	else if (!any_on)
		set_shown(q, -1, 0);
}

static void *worker(void *arg)
{
	queue_ctx_t *q = arg;

	for (;;) {
		struct job job;
		int win_x = 0, win_y = 0;
		bool viewable;
		VdpTime shown_at = 0;

		pthread_mutex_lock(&q->lock);
		while (!q->count && !q->quit)
			pthread_cond_wait(&q->cond, &q->lock);
		if (q->quit) {
			pthread_mutex_unlock(&q->lock);
			break;
		}
		job = q->jobs[q->head];
		q->head = (q->head + 1) % MAX_JOBS;
		q->count--;
		q->busy = job.os;
		pthread_mutex_unlock(&q->lock);

		if (job.os) {
			/* geometry first: the X round trips stay off the deadline */
			viewable = window_geometry(q, &win_x, &win_y);

			if (job.earliest > now_ns()) {
				struct timespec ts = {
					.tv_sec = job.earliest / 1000000000ull,
					.tv_nsec = job.earliest % 1000000000ull,
				};

				while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
					;
			}

			pthread_mutex_lock(&q->device->drm_lock);
			show(q, job.os, job.clip_w, job.clip_h, win_x, win_y, viewable);
			/* legacy SETPLANE completes at the vblank that shows the change */
			shown_at = q->crtc >= 0 ? drm_last_vblank(q->device, q->crtc) : 0;
			pthread_mutex_unlock(&q->device->drm_lock);
			if (!shown_at)
				shown_at = now_ns();
		}

		pthread_mutex_lock(&q->lock);
		if (job.os) {
			job.os->pending--;
			if (q->visible && q->visible != job.os)
				q->visible->status = VDP_PRESENTATION_QUEUE_STATUS_IDLE;
			q->visible = job.os;
			job.os->status = job.os->pending ? VDP_PRESENTATION_QUEUE_STATUS_QUEUED :
							   VDP_PRESENTATION_QUEUE_STATUS_VISIBLE;
			job.os->first_presentation_time = shown_at;
		}
		q->busy = NULL;
		pthread_cond_broadcast(&q->cond);
		pthread_mutex_unlock(&q->lock);
	}
	return NULL;
}

VdpStatus vdp_presentation_queue_create(VdpDevice device, VdpPresentationQueueTarget target,
					VdpPresentationQueue *queue)
{
	device_ctx_t *dev = handle_get(device);
	queue_target_ctx_t *t = handle_get(target);
	queue_ctx_t *q;

	if (!queue)
		return VDP_STATUS_INVALID_POINTER;
	if (!dev || !t)
		return VDP_STATUS_INVALID_HANDLE;
	if (dev->queue)
		return VDP_STATUS_RESOURCES;	/* one set of planes per device */
	if (drm_open(dev))
		return VDP_STATUS_ERROR;

	q = calloc(1, sizeof(*q));
	if (!q)
		return VDP_STATUS_RESOURCES;
	q->device = dev;
	q->target = t;
	q->shown_buf = -1;
	q->crtc = -1;
	q->xdpy = XOpenDisplay(DisplayString(dev->display));
	if (!q->xdpy) {
		free(q);
		return VDP_STATUS_ERROR;
	}
	pthread_mutex_init(&q->lock, NULL);
	pthread_cond_init(&q->cond, NULL);
	if (pthread_create(&q->thread, NULL, worker, q)) {
		XCloseDisplay(q->xdpy);
		free(q);
		return VDP_STATUS_RESOURCES;
	}

	*queue = handle_create(q);
	if (*queue == VDP_INVALID_HANDLE) {
		pthread_mutex_lock(&q->lock);
		q->quit = true;
		pthread_cond_broadcast(&q->cond);
		pthread_mutex_unlock(&q->lock);
		pthread_join(q->thread, NULL);
		XCloseDisplay(q->xdpy);
		free(q);
		return VDP_STATUS_RESOURCES;
	}
	dev->queue = q;
	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_destroy(VdpPresentationQueue queue)
{
	queue_ctx_t *q = handle_get(queue);

	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	pthread_mutex_lock(&q->lock);
	q->quit = true;
	pthread_cond_broadcast(&q->cond);
	pthread_mutex_unlock(&q->lock);
	pthread_join(q->thread, NULL);

	pthread_mutex_lock(&q->device->drm_lock);
	planes_off(q);
	set_shown(q, -1, 0);
	pthread_mutex_unlock(&q->device->drm_lock);

	/* jobs never shown still count on their surfaces */
	for (unsigned int i = 0; i < q->count; i++) {
		struct job *j = &q->jobs[(q->head + i) % MAX_JOBS];

		if (j->os) {
			j->os->pending--;
			j->os->status = VDP_PRESENTATION_QUEUE_STATUS_IDLE;
		}
	}
	if (q->visible)
		q->visible->status = VDP_PRESENTATION_QUEUE_STATUS_IDLE;

	q->device->queue = NULL;
	XCloseDisplay(q->xdpy);
	pthread_cond_destroy(&q->cond);
	pthread_mutex_destroy(&q->lock);
	handle_destroy(queue);
	free(q);
	return VDP_STATUS_OK;
}

void presentation_forget_surface(device_ctx_t *dev, output_surface_ctx_t *os)
{
	queue_ctx_t *q = dev->queue;

	if (!q)
		return;
	pthread_mutex_lock(&q->lock);
	/* cancel its queued jobs; wait only for one being shown right now */
	for (unsigned int i = 0; i < q->count; i++) {
		struct job *j = &q->jobs[(q->head + i) % MAX_JOBS];

		if (j->os == os) {
			j->os = NULL;
			os->pending--;
		}
	}
	while (q->busy == os)
		pthread_cond_wait(&q->cond, &q->lock);
	if (q->visible == os) {
		q->visible = NULL;
		pthread_mutex_lock(&dev->drm_lock);
		if (planes_off(q))
			set_shown(q, -1, 0);
		pthread_mutex_unlock(&dev->drm_lock);
	}
	pthread_mutex_unlock(&q->lock);
}

VdpStatus vdp_presentation_queue_set_background_color(VdpPresentationQueue queue,
						      VdpColor *const background_color)
{
	queue_ctx_t *q = handle_get(queue);

	if (!background_color)
		return VDP_STATUS_INVALID_POINTER;
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;
	q->background = *background_color;	/* would be the colour key */
	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_get_background_color(VdpPresentationQueue queue,
						      VdpColor *background_color)
{
	queue_ctx_t *q = handle_get(queue);

	if (!background_color)
		return VDP_STATUS_INVALID_POINTER;
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;
	*background_color = q->background;
	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_get_time(VdpPresentationQueue queue, VdpTime *current_time)
{
	if (!current_time)
		return VDP_STATUS_INVALID_POINTER;
	if (!handle_get(queue))
		return VDP_STATUS_INVALID_HANDLE;
	*current_time = now_ns();
	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_display(VdpPresentationQueue queue, VdpOutputSurface surface,
					 uint32_t clip_width, uint32_t clip_height,
					 VdpTime earliest_presentation_time)
{
	queue_ctx_t *q = handle_get(queue);
	output_surface_ctx_t *os = handle_get(surface);
	struct job *j;

	if (!q || !os)
		return VDP_STATUS_INVALID_HANDLE;
	if (os->device != q->device)
		return VDP_STATUS_HANDLE_DEVICE_MISMATCH;

	pthread_mutex_lock(&q->lock);
	while (q->count == MAX_JOBS)
		pthread_cond_wait(&q->cond, &q->lock);
	j = &q->jobs[(q->head + q->count) % MAX_JOBS];
	j->os = os;
	j->clip_w = clip_width ? clip_width : os->width;
	j->clip_h = clip_height ? clip_height : os->height;
	j->earliest = earliest_presentation_time;
	q->count++;
	os->pending++;
	os->status = VDP_PRESENTATION_QUEUE_STATUS_QUEUED;
	pthread_cond_broadcast(&q->cond);
	pthread_mutex_unlock(&q->lock);
	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_block_until_surface_idle(VdpPresentationQueue queue,
							  VdpOutputSurface surface,
							  VdpTime *first_presentation_time)
{
	queue_ctx_t *q = handle_get(queue);
	output_surface_ctx_t *os = handle_get(surface);
	struct timespec deadline;
	VdpStatus ret = VDP_STATUS_OK;

	if (!first_presentation_time)
		return VDP_STATUS_INVALID_POINTER;
	if (!q || !os)
		return VDP_STATUS_INVALID_HANDLE;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 2;	/* never hang a player on a wedged display */
	pthread_mutex_lock(&q->lock);
	/*
	 * VISIBLE stays VISIBLE until another surface is shown; with nothing
	 * else queued that would never happen, so a visible surface with an
	 * empty queue counts as idle. Its picture stays safe: the worker holds
	 * its own reference on the buffer on the plane.
	 */
	while (os->status != VDP_PRESENTATION_QUEUE_STATUS_IDLE &&
	       !(os->status == VDP_PRESENTATION_QUEUE_STATUS_VISIBLE && !q->count && !q->busy)) {
		if (pthread_cond_timedwait(&q->cond, &q->lock, &deadline) == ETIMEDOUT) {
			ERR("presentation queue: surface not idle after 2 s\n");
			ret = VDP_STATUS_ERROR;
			break;
		}
	}
	*first_presentation_time = os->first_presentation_time;
	pthread_mutex_unlock(&q->lock);
	return ret;
}

VdpStatus vdp_presentation_queue_query_surface_status(VdpPresentationQueue queue,
						      VdpOutputSurface surface,
						      VdpPresentationQueueStatus *status,
						      VdpTime *first_presentation_time)
{
	queue_ctx_t *q = handle_get(queue);
	output_surface_ctx_t *os = handle_get(surface);

	if (!status || !first_presentation_time)
		return VDP_STATUS_INVALID_POINTER;
	if (!q || !os)
		return VDP_STATUS_INVALID_HANDLE;
	pthread_mutex_lock(&q->lock);
	*status = os->status;
	*first_presentation_time = os->first_presentation_time;
	pthread_mutex_unlock(&q->lock);
	return VDP_STATUS_OK;
}
