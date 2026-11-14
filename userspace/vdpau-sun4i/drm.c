// SPDX-License-Identifier: MIT
// Copyright (c) 2026 D.R.racer <drracer@seznam.cz>
/*
 * DRM side: an X-authenticated card0 fd, the overlay planes of the CRTC
 * showing the X screen, framebuffers for decoded pictures (cedrus CAPTURE
 * buffers, tiled NV12, imported as DMA-BUF) and dumb ARGB buffers (OSD).
 *
 * Needs the local sun4i patch that lets an authenticated non-master show
 * its own framebuffers on designated overlay planes.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <X11/Xlib-xcb.h>
#include <xcb/dri2.h>

#include "vdpau_private.h"

#define ALIGN32(x)	(((x) + 31) & ~31U)

static int authenticate(int fd, Display *display, int screen)
{
	xcb_connection_t *c = XGetXCBConnection(display);
	xcb_dri2_authenticate_reply_t *r;
	drm_magic_t magic;
	int ok;

	if (drmGetMagic(fd, &magic))
		return -1;
	r = xcb_dri2_authenticate_reply(c, xcb_dri2_authenticate(c, RootWindow(display, screen),
								 magic), NULL);
	ok = r && r->authenticated;
	free(r);
	return ok ? 0 : -1;
}

/* Overlay planes usable on `crtc_index`: video = lowest zpos, OSD = highest. */
static void find_planes(struct drm_state *drm, struct drm_crtc_info *crtc)
{
	drmModePlaneRes *pres = drmModeGetPlaneResources(drm->fd);
	uint32_t overlays[8];
	uint64_t zpos[8];
	unsigned int n = 0;

	for (uint32_t i = 0; pres && i < pres->count_planes && n < 8; i++) {
		drmModePlane *p = drmModeGetPlane(drm->fd, pres->planes[i]);
		drmModeObjectProperties *props = drmModeObjectGetProperties(drm->fd,
				pres->planes[i], DRM_MODE_OBJECT_PLANE);
		uint64_t type = 0, z = i;

		for (uint32_t j = 0; props && j < props->count_props; j++) {
			drmModePropertyRes *pr = drmModeGetProperty(drm->fd, props->props[j]);

			if (pr && !strcmp(pr->name, "type"))
				type = props->prop_values[j];
			if (pr && !strcmp(pr->name, "zpos"))
				z = props->prop_values[j];
			drmModeFreeProperty(pr);
		}
		if (p && type == DRM_PLANE_TYPE_OVERLAY &&
		    (p->possible_crtcs & (1u << crtc->index))) {
			/* insertion sort by zpos */
			unsigned int k = n++;

			while (k && zpos[k - 1] > z) {
				overlays[k] = overlays[k - 1];
				zpos[k] = zpos[k - 1];
				k--;
			}
			overlays[k] = p->plane_id;
			zpos[k] = z;
		}
		drmModeFreeObjectProperties(props);
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(pres);

	crtc->video_plane = n ? overlays[0] : 0;
	crtc->osd_plane = n > 1 ? overlays[n - 1] : 0;
}

int drm_open(device_ctx_t *dev)
{
	struct drm_state *drm = &dev->drm;
	drmModeRes *res;

	if (drm->fd >= 0)
		return 0;

	drm->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm->fd < 0) {
		ERR("cannot open /dev/dri/card0: %s\n", strerror(errno));
		return -1;
	}
	if (authenticate(drm->fd, dev->display, dev->screen)) {
		ERR("X server refused DRI2 authentication\n");
		goto fail;
	}
	drmSetClientCap(drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	/*
	 * Each CRTC (A20: two backend/TCON pipelines) has its own overlay
	 * planes; with several monitors the X screen spans them, and the
	 * video goes to whichever one shows the window.
	 */
	res = drmModeGetResources(drm->fd);
	for (int i = 0; res && i < res->count_crtcs && drm->ncrtcs < MAX_CRTCS; i++) {
		struct drm_crtc_info *c = &drm->crtcs[drm->ncrtcs++];

		c->id = res->crtcs[i];
		c->index = i;
		find_planes(drm, c);
	}
	drmModeFreeResources(res);
	/* monitors may all be off (DPMS) right now: placement waits for one */
	drm_update_crtcs(dev);
	for (int i = 0; i < drm->ncrtcs; i++) {
		const struct drm_crtc_info *c = &drm->crtcs[i];

		DBG("DRM: crtc %u %s %ux%u+%d+%d, video plane %u, OSD plane %u\n", c->id,
		    c->active ? "on" : "off", c->w, c->h, c->x, c->y, c->video_plane,
		    c->osd_plane);
		if (c->video_plane)
			return 0;
	}
	ERR("no overlay plane\n");
	goto fail;

fail:
	close(drm->fd);
	drm->fd = -1;
	return -1;
}

void drm_close(device_ctx_t *dev)
{
	if (dev->drm.fd >= 0)
		close(dev->drm.fd);
	dev->drm.fd = -1;
}

int drm_update_crtcs(device_ctx_t *dev)
{
	struct drm_state *drm = &dev->drm;
	int ret = -1;

	for (int i = 0; i < drm->ncrtcs; i++) {
		struct drm_crtc_info *ci = &drm->crtcs[i];
		drmModeCrtc *c = drmModeGetCrtc(drm->fd, ci->id);

		ci->active = c && c->mode_valid;
		if (ci->active) {
			ci->x = c->x;
			ci->y = c->y;
			ci->w = c->mode.hdisplay;
			ci->h = c->mode.vdisplay;
			ret = 0;
		}
		drmModeFreeCrtc(c);
	}
	return ret;
}

static int64_t overlap(const struct drm_crtc_info *c, int x, int y, uint32_t w, uint32_t h)
{
	int64_t x0 = x > c->x ? x : c->x, y0 = y > c->y ? y : c->y;
	int64_t x1 = (int64_t)x + w, y1 = (int64_t)y + h;

	if (x1 > (int64_t)c->x + c->w)
		x1 = (int64_t)c->x + c->w;
	if (y1 > (int64_t)c->y + c->h)
		y1 = (int64_t)c->y + c->h;
	return x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) : 0;
}

int drm_pick_crtc(device_ctx_t *dev, int cur, int x, int y, uint32_t w, uint32_t h)
{
	struct drm_state *drm = &dev->drm;
	int64_t best_area = -1;
	int best = -1;

	for (int i = 0; i < drm->ncrtcs; i++) {
		const struct drm_crtc_info *c = &drm->crtcs[i];
		int64_t a;

		if (!c->active || !c->video_plane)
			continue;
		a = overlap(c, x, y, w, h);
		/* on a tie (window off all screens) keep the current CRTC */
		if (a > best_area || (a == best_area && i == cur)) {
			best = i;
			best_area = a;
		}
	}
	return best;
}

VdpTime drm_last_vblank(device_ctx_t *dev, int crtc)
{
	drmVBlank vbl = {
		.request.type = DRM_VBLANK_RELATIVE |
				(dev->drm.crtcs[crtc].index << DRM_VBLANK_HIGH_CRTC_SHIFT),
		.request.sequence = 0,
	};

	if (drmWaitVBlank(dev->drm.fd, &vbl))
		return 0;
	return (VdpTime)vbl.reply.tval_sec * 1000000000ull + (VdpTime)vbl.reply.tval_usec * 1000;
}

/* Framebuffer of a decoded picture (cached per CAPTURE buffer). */
uint32_t drm_video_fb(device_ctx_t *dev, decoder_ctx_t *dec, unsigned int index)
{
	struct drm_state *drm = &dev->drm;
	struct capture_buffer *cb = &dec->capture[index];
	struct v4l2_exportbuffer exp = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = index, .flags = O_RDONLY | O_CLOEXEC,
	};
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 }, fb = 0;
	uint64_t mods[4] = { 0 };
	uint32_t gem;

	if (cb->fb_id)
		return cb->fb_id;
	if (ioctl(dev->video_fd, VIDIOC_EXPBUF, &exp)) {
		ERR("EXPBUF %u: %s\n", index, strerror(errno));
		return 0;
	}
	if (drmPrimeFDToHandle(drm->fd, exp.fd, &gem)) {
		ERR("PRIME import: %s\n", strerror(errno));
		close(exp.fd);
		return 0;
	}
	close(exp.fd);

	handles[0] = handles[1] = gem;
	pitches[0] = pitches[1] = dec->cap_stride;
	offsets[1] = dec->cap_stride * ALIGN32(dec->cap_height);
	mods[0] = mods[1] = DRM_FORMAT_MOD_ALLWINNER_TILED;
	if (drmModeAddFB2WithModifiers(drm->fd, dec->width, dec->height, DRM_FORMAT_NV12, handles,
				       pitches, offsets, mods, &fb, DRM_MODE_FB_MODIFIERS)) {
		ERR("ADDFB2 tiled NV12: %s\n", strerror(errno));
		fb = 0;
	}
	cb->fb_id = fb;
	cb->gem_handle = gem;
	return fb;
}

void drm_release_video_fbs(device_ctx_t *dev, decoder_ctx_t *dec)
{
	for (unsigned int i = 0; i < dec->capture_count; i++) {
		struct capture_buffer *cb = &dec->capture[i];

		if (cb->fb_id)
			drmModeRmFB(dev->drm.fd, cb->fb_id);
		if (cb->gem_handle) {
			struct drm_gem_close gc = { .handle = cb->gem_handle };

			drmIoctl(dev->drm.fd, DRM_IOCTL_GEM_CLOSE, &gc);
		}
		cb->fb_id = cb->gem_handle = 0;
	}
}

int drm_dumb_create(device_ctx_t *dev, uint32_t width, uint32_t height, struct dumb_buffer *b)
{
	struct drm_mode_create_dumb create = { .width = width, .height = height, .bpp = 32 };
	struct drm_mode_map_dumb map = { 0 };
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	memset(b, 0, sizeof(*b));
	if (drmIoctl(dev->drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create)) {
		ERR("dumb buffer %ux%u: %s\n", width, height, strerror(errno));
		return -1;
	}
	b->handle = create.handle;
	b->pitch = create.pitch;
	b->size = create.size;
	map.handle = b->handle;
	if (drmIoctl(dev->drm.fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
		goto fail;
	b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->drm.fd, map.offset);
	if (b->map == MAP_FAILED) {
		b->map = NULL;
		goto fail;
	}
	handles[0] = b->handle;
	pitches[0] = b->pitch;
	if (drmModeAddFB2(dev->drm.fd, width, height, DRM_FORMAT_ARGB8888, handles, pitches,
			  offsets, &b->fb_id, 0)) {
		ERR("ADDFB2 ARGB: %s\n", strerror(errno));
		goto fail;
	}
	memset(b->map, 0, b->size);
	return 0;

fail:
	drm_dumb_destroy(dev, b);
	return -1;
}

void drm_dumb_destroy(device_ctx_t *dev, struct dumb_buffer *b)
{
	struct drm_mode_destroy_dumb d = { .handle = b->handle };

	if (b->fb_id)
		drmModeRmFB(dev->drm.fd, b->fb_id);
	if (b->map)
		munmap(b->map, b->size);
	if (b->handle)
		drmIoctl(dev->drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	memset(b, 0, sizeof(*b));
}

int drm_show(device_ctx_t *dev, uint32_t crtc, uint32_t plane, uint32_t fb, int x, int y, unsigned int w,
	     unsigned int h, unsigned int sx, unsigned int sy, unsigned int sw, unsigned int sh)
{
	if (!plane)
		return -1;
	if (!fb)
		return drmModeSetPlane(dev->drm.fd, plane, crtc, 0, 0,
				       0, 0, 0, 0, 0, 0, 0, 0);
	return drmModeSetPlane(dev->drm.fd, plane, crtc, fb, 0, x, y, w, h,
			       sx << 16, sy << 16, sw << 16, sh << 16);
}
