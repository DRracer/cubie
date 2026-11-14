/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 D.R.racer <drracer@seznam.cz> */
/*
 * gstsun4idrm.c — DRM helper for sun4iximagesink
 *
 * Handles DRM device open, plane discovery, DMA-BUF import, and
 * framebuffer management. FB IDs are cached on GstMemory via qdata
 * so that steady-state rendering does zero DRM import calls.
 */

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <X11/Xlib-xcb.h>
#include <xcb/dri2.h>

#include "gstsun4idrm.h"

GST_DEBUG_CATEGORY_EXTERN (sun4i_ximagesink_debug);
#define GST_CAT_DEFAULT sun4i_ximagesink_debug

/* Quark for caching fb_id on GstMemory */
static GQuark fb_id_quark;

/*
 * The decoder's buffers (and the FBs cached on their memory) can outlive
 * stop(): the fd is closed only when the sink and every cached FB are done.
 */
typedef struct _Sun4iDrmDev {
  gint ref;
  int fd;
} Sun4iDrmDev;

static void
drm_dev_unref (Sun4iDrmDev *dev)
{
  if (g_atomic_int_dec_and_test (&dev->ref)) {
    close (dev->fd);
    g_free (dev);
  }
}

typedef struct {
  Sun4iDrmDev *dev;
  guint32 fb_id;
  guint32 gem_handle;
} CachedFB;

static void
cached_fb_destroy (gpointer data)
{
  CachedFB *fb = data;
  if (fb->fb_id)
    drmModeRmFB (fb->dev->fd, fb->fb_id);
  if (fb->gem_handle) {
    struct drm_gem_close gem_close = { .handle = fb->gem_handle };
    ioctl (fb->dev->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
  }
  drm_dev_unref (fb->dev);
  g_free (fb);
}

/* --- Open / Close --- */

gboolean
sun4i_drm_open (GstSun4iXImageSink *self)
{
  guint64 cap = 0;

  fb_id_quark = g_quark_from_static_string ("sun4i-drm-fb-id");

  self->drm_fd = open ("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (self->drm_fd < 0) {
    GST_ERROR_OBJECT (self, "Failed to open /dev/dri/card0: %s",
        g_strerror (errno));
    return FALSE;
  }

  /*
   * Xorg holds DRM master; the kernel lets an authenticated client show its
   * own framebuffers on designated overlay planes. Authenticate via DRI2.
   */
  {
    xcb_connection_t *xcb = XGetXCBConnection (self->xdisplay);
    xcb_window_t root = DefaultRootWindow (self->xdisplay);
    xcb_dri2_authenticate_reply_t *auth;
    drm_magic_t magic;

    if (drmGetMagic (self->drm_fd, &magic) != 0) {
      GST_ERROR_OBJECT (self, "drmGetMagic failed: %s", g_strerror (errno));
      goto fail;
    }
    auth = xcb_dri2_authenticate_reply (xcb,
        xcb_dri2_authenticate (xcb, root, magic), NULL);
    if (!auth || !auth->authenticated) {
      GST_ERROR_OBJECT (self, "X server refused DRI2 authentication");
      free (auth);
      goto fail;
    }
    free (auth);
  }

  /* Enable universal planes so we can see overlays */
  if (drmSetClientCap (self->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
    GST_WARNING_OBJECT (self, "DRM_CLIENT_CAP_UNIVERSAL_PLANES not supported");
  }

  /* Enable atomic (needed for some property setting) */
  if (drmSetClientCap (self->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
    GST_DEBUG_OBJECT (self, "DRM_CLIENT_CAP_ATOMIC not supported, using legacy");
  }

  /* Check PRIME import support */
  if (drmGetCap (self->drm_fd, DRM_CAP_PRIME, &cap) == 0
      && (cap & DRM_PRIME_CAP_IMPORT)) {
    GST_DEBUG_OBJECT (self, "DRM PRIME import supported");
  } else {
    GST_ERROR_OBJECT (self, "DRM PRIME import not supported");
    close (self->drm_fd);
    self->drm_fd = -1;
    return FALSE;
  }

  /* Check AddFB2 modifiers support */
  self->has_modifiers = FALSE;
  if (drmGetCap (self->drm_fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap) {
    self->has_modifiers = TRUE;
    GST_DEBUG_OBJECT (self, "AddFB2 with modifiers supported");
  }

  self->drm_dev = g_new0 (Sun4iDrmDev, 1);
  self->drm_dev->ref = 1;
  self->drm_dev->fd = self->drm_fd;

  GST_INFO_OBJECT (self, "DRM device opened: fd=%d, modifiers=%s",
      self->drm_fd, self->has_modifiers ? "yes" : "no");
  return TRUE;

fail:
  close (self->drm_fd);
  self->drm_fd = -1;
  return FALSE;
}

void
sun4i_drm_close (GstSun4iXImageSink *self)
{
  if (self->drm_dev) {
    drm_dev_unref (self->drm_dev);
    self->drm_dev = NULL;
  }
  self->drm_fd = -1;
}

/* --- Find Resources --- */

/*
 * Check if a plane supports a given DRM format.
 */
static gboolean
plane_supports_format (int drm_fd, guint32 plane_id, guint32 fourcc)
{
  drmModePlane *plane = drmModeGetPlane (drm_fd, plane_id);
  if (!plane)
    return FALSE;

  for (guint i = 0; i < plane->count_formats; i++) {
    if (plane->formats[i] == fourcc) {
      drmModeFreePlane (plane);
      return TRUE;
    }
  }
  drmModeFreePlane (plane);
  return FALSE;
}

/*
 * Get the "type" property of a plane (Primary=1, Overlay=2, Cursor=3).
 */
static guint64
get_plane_type (int drm_fd, guint32 plane_id)
{
  drmModeObjectProperties *props;
  guint64 type = 0;

  props = drmModeObjectGetProperties (drm_fd, plane_id,
      DRM_MODE_OBJECT_PLANE);
  if (!props)
    return 0;

  for (guint i = 0; i < props->count_props; i++) {
    drmModePropertyRes *prop = drmModeGetProperty (drm_fd, props->props[i]);
    if (prop) {
      if (g_strcmp0 (prop->name, "type") == 0) {
        type = props->prop_values[i];
        drmModeFreeProperty (prop);
        break;
      }
      drmModeFreeProperty (prop);
    }
  }
  drmModeFreeProperty (NULL);  /* no-op, just for symmetry */
  drmModeFreeObjectProperties (props);
  return type;
}

gboolean
sun4i_drm_find_resources (GstSun4iXImageSink *self)
{
  drmModeRes *res = NULL;
  drmModePlaneRes *plane_res = NULL;
  drmModeConnector *connector = NULL;
  drmModeEncoder *encoder = NULL;
  guint32 crtc_id = 0;
  guint32 found_plane = 0;

  res = drmModeGetResources (self->drm_fd);
  if (!res) {
    GST_ERROR_OBJECT (self, "drmModeGetResources failed");
    return FALSE;
  }

  /* Find connected connector */
  for (int i = 0; i < res->count_connectors; i++) {
    if (self->connector_id && (guint32) self->connector_id != res->connectors[i])
      continue;

    connector = drmModeGetConnector (self->drm_fd, res->connectors[i]);
    if (connector && connector->connection == DRM_MODE_CONNECTED) {
      self->drm_connector_id = connector->connector_id;
      break;
    }
    drmModeFreeConnector (connector);
    connector = NULL;
  }

  if (!connector) {
    GST_ERROR_OBJECT (self, "No connected connector found");
    goto fail;
  }

  /* Find the encoder → CRTC */
  if (connector->encoder_id) {
    encoder = drmModeGetEncoder (self->drm_fd, connector->encoder_id);
    if (encoder)
      crtc_id = encoder->crtc_id;
  }

  if (!crtc_id && connector->count_encoders > 0) {
    /* Try first available encoder */
    encoder = drmModeGetEncoder (self->drm_fd, connector->encoders[0]);
    if (encoder) {
      for (int i = 0; i < res->count_crtcs; i++) {
        if (encoder->possible_crtcs & (1u << i)) {
          crtc_id = res->crtcs[i];
          break;
        }
      }
    }
  }

  if (!crtc_id) {
    GST_ERROR_OBJECT (self, "No CRTC found for connector %u",
        self->drm_connector_id);
    goto fail;
  }
  self->drm_crtc_id = crtc_id;

  /* Get current mode for screen dimensions */
  {
    drmModeCrtc *crtc = drmModeGetCrtc (self->drm_fd, crtc_id);
    if (crtc && crtc->mode_valid) {
      self->hdisplay = crtc->mode.hdisplay;
      self->vdisplay = crtc->mode.vdisplay;
      GST_INFO_OBJECT (self, "CRTC %u mode: %ux%u", crtc_id,
          self->hdisplay, self->vdisplay);
    } else {
      GST_ERROR_OBJECT (self, "CRTC %u has no valid mode", crtc_id);
      drmModeFreeCrtc (crtc);
      goto fail;
    }
    drmModeFreeCrtc (crtc);
  }

  /* Find an overlay plane that supports NV12 */
  plane_res = drmModeGetPlaneResources (self->drm_fd);
  if (!plane_res) {
    GST_ERROR_OBJECT (self, "drmModeGetPlaneResources failed");
    goto fail;
  }

  for (guint i = 0; i < plane_res->count_planes; i++) {
    guint32 pid = plane_res->planes[i];

    /* If user specified a plane, skip others */
    if (self->plane_id && (guint32) self->plane_id != pid)
      continue;

    /* Must be an overlay plane (type == 2) */
    if (get_plane_type (self->drm_fd, pid) != DRM_PLANE_TYPE_OVERLAY)
      continue;

    /* Must support NV12 */
    if (!plane_supports_format (self->drm_fd, pid, DRM_FORMAT_NV12))
      continue;

    /* Check it can be used with our CRTC */
    {
      drmModePlane *plane = drmModeGetPlane (self->drm_fd, pid);
      if (plane) {
        guint32 crtc_idx = 0;
        for (int c = 0; c < res->count_crtcs; c++) {
          if (res->crtcs[c] == crtc_id) {
            crtc_idx = c;
            break;
          }
        }
        if (plane->possible_crtcs & (1u << crtc_idx)) {
          found_plane = pid;
          drmModeFreePlane (plane);
          break;
        }
        drmModeFreePlane (plane);
      }
    }
  }

  if (!found_plane) {
    GST_ERROR_OBJECT (self, "No NV12-capable overlay plane found for CRTC %u",
        crtc_id);
    goto fail;
  }

  self->drm_plane_id = found_plane;

  GST_INFO_OBJECT (self, "Using connector=%u, crtc=%u, plane=%u, mode=%ux%u",
      self->drm_connector_id, self->drm_crtc_id, self->drm_plane_id,
      self->hdisplay, self->vdisplay);

  drmModeFreePlaneResources (plane_res);
  drmModeFreeEncoder (encoder);
  drmModeFreeConnector (connector);
  drmModeFreeResources (res);
  return TRUE;

fail:
  if (plane_res) drmModeFreePlaneResources (plane_res);
  if (encoder) drmModeFreeEncoder (encoder);
  if (connector) drmModeFreeConnector (connector);
  if (res) drmModeFreeResources (res);
  return FALSE;
}

/* --- Plane zpos --- */

gboolean
sun4i_drm_set_plane_zpos (GstSun4iXImageSink *self, guint32 plane_id,
    gint zpos)
{
  drmModeObjectProperties *props;
  gboolean found = FALSE;

  props = drmModeObjectGetProperties (self->drm_fd, plane_id,
      DRM_MODE_OBJECT_PLANE);
  if (!props)
    return FALSE;

  for (guint i = 0; i < props->count_props; i++) {
    drmModePropertyRes *prop = drmModeGetProperty (self->drm_fd, props->props[i]);
    if (prop) {
      if (g_strcmp0 (prop->name, "zpos") == 0) {
        int ret = drmModeObjectSetProperty (self->drm_fd, plane_id,
            DRM_MODE_OBJECT_PLANE, prop->prop_id, zpos);
        if (ret == 0) {
          GST_INFO_OBJECT (self, "Set plane %u zpos=%d", plane_id, zpos);
          found = TRUE;
        } else {
          GST_WARNING_OBJECT (self, "Failed to set zpos: %s", g_strerror (-ret));
        }
        drmModeFreeProperty (prop);
        break;
      }
      drmModeFreeProperty (prop);
    }
  }
  drmModeFreeObjectProperties (props);
  return found;
}

/* --- DMA-BUF import with FB caching --- */

guint32
sun4i_drm_import_dmabuf (GstSun4iXImageSink *self, GstBuffer *buf)
{
  GstMemory *mem;
  CachedFB *cached;
  int fd;
  guint32 gem_handle = 0;
  guint32 fb_id = 0;
  guint32 handles[4] = { 0 };
  guint32 pitches[4] = { 0 };
  guint32 offsets[4] = { 0 };
  guint64 modifiers[4] = { 0 };
  int ret;

  mem = gst_buffer_peek_memory (buf, 0);
  if (!mem || !gst_is_dmabuf_memory (mem)) {
    GST_ERROR_OBJECT (self, "Buffer is not DMA-BUF");
    return 0;
  }

  /* Check cache */
  cached = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem), fb_id_quark);
  if (cached && cached->dev == self->drm_dev) {
    return cached->fb_id;
  }

  fd = gst_dmabuf_memory_get_fd (mem);
  if (fd < 0) {
    GST_ERROR_OBJECT (self, "Failed to get DMA-BUF fd");
    return 0;
  }

  /* Import DMA-BUF fd → GEM handle */
  ret = drmPrimeFDToHandle (self->drm_fd, fd, &gem_handle);
  if (ret != 0) {
    GST_ERROR_OBJECT (self, "drmPrimeFDToHandle failed: %s",
        g_strerror (errno));
    return 0;
  }

  /* NV12: plane 0 = Y, plane 1 = UV interleaved
   * For tiled format, the pitch needs special handling:
   * The hardware uses the stride from the DRM FB, and for tiled NV12
   * the stride should reflect the tile-aligned width. With the Cedrus
   * decoder, buffers are allocated with tile-aligned stride.
   */
  gint video_w = GST_VIDEO_INFO_WIDTH (&self->vinfo);
  gint video_h = GST_VIDEO_INFO_HEIGHT (&self->vinfo);

  /* Get stride info from GstVideoMeta if available */
  GstVideoMeta *vmeta = gst_buffer_get_video_meta (buf);
  if (vmeta) {
    const GstVideoFormatInfo *finfo = gst_video_format_get_info (vmeta->format);

    for (guint i = 0; i < 2; i++) {
      pitches[i] = vmeta->stride[i];
      offsets[i] = vmeta->offset[i];
      /* Tiled formats carry packed tile counts, DRM wants bytes per row. */
      if (GST_VIDEO_FORMAT_INFO_IS_TILED (finfo))
        pitches[i] = GST_VIDEO_TILE_X_TILES (vmeta->stride[i]) *
            GST_VIDEO_FORMAT_INFO_TILE_STRIDE (finfo, i);
    }
  } else {
    /* Fallback: assume tiled-aligned stride = ALIGN(width, 32) */
    guint aligned_w = GST_ROUND_UP_32 (video_w);
    pitches[0] = aligned_w;
    pitches[1] = aligned_w;
    offsets[0] = 0;
    offsets[1] = aligned_w * GST_ROUND_UP_32 (video_h);
  }

  handles[0] = gem_handle;
  handles[1] = gem_handle;

  if (self->has_modifiers && self->drm_modifier != DRM_FORMAT_MOD_LINEAR) {
    modifiers[0] = self->drm_modifier;
    modifiers[1] = self->drm_modifier;

    ret = drmModeAddFB2WithModifiers (self->drm_fd,
        video_w, video_h, self->drm_fourcc,
        handles, pitches, offsets, modifiers,
        &fb_id, DRM_MODE_FB_MODIFIERS);
  } else {
    ret = drmModeAddFB2 (self->drm_fd,
        video_w, video_h, self->drm_fourcc,
        handles, pitches, offsets,
        &fb_id, 0);
  }

  if (ret != 0) {
    GST_ERROR_OBJECT (self, "drmModeAddFB2 failed: %s (w=%d h=%d fourcc=0x%08x "
        "pitch0=%u pitch1=%u off0=%u off1=%u mod=0x%016" G_GINT64_MODIFIER "x)",
        g_strerror (errno), video_w, video_h, self->drm_fourcc,
        pitches[0], pitches[1], offsets[0], offsets[1], self->drm_modifier);
    struct drm_gem_close gem_close = { .handle = gem_handle };
    ioctl (self->drm_fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
    return 0;
  }

  /* Cache on GstMemory for reuse */
  cached = g_new0 (CachedFB, 1);
  cached->dev = self->drm_dev;
  g_atomic_int_inc (&self->drm_dev->ref);
  cached->fb_id = fb_id;
  cached->gem_handle = gem_handle;
  gst_mini_object_set_qdata (GST_MINI_OBJECT (mem), fb_id_quark,
      cached, cached_fb_destroy);

  GST_LOG_OBJECT (self, "Imported DMA-BUF fd=%d → gem=%u → fb=%u",
      fd, gem_handle, fb_id);

  return fb_id;
}

/* --- Show / Hide plane --- */

gboolean
sun4i_drm_show_plane (GstSun4iXImageSink *self, guint32 fb_id,
    gint dst_x, gint dst_y, gint dst_w, gint dst_h,
    gint src_x, gint src_y, gint src_w, gint src_h)
{
  int ret;

  ret = drmModeSetPlane (self->drm_fd, self->drm_plane_id, self->drm_crtc_id,
      fb_id, 0,
      dst_x, dst_y, dst_w, dst_h,
      src_x, src_y, src_w, src_h);

  if (ret != 0) {
    GST_ERROR_OBJECT (self, "drmModeSetPlane failed: %s (plane=%u crtc=%u fb=%u "
        "dst=%d,%d+%dx%d src=%d,%d+%dx%d)",
        g_strerror (errno),
        self->drm_plane_id, self->drm_crtc_id, fb_id,
        dst_x, dst_y, dst_w, dst_h,
        src_x >> 16, src_y >> 16, src_w >> 16, src_h >> 16);
    return FALSE;
  }

  self->plane_visible = TRUE;
  return TRUE;
}

gboolean
sun4i_drm_hide_plane (GstSun4iXImageSink *self)
{
  int ret;

  if (self->drm_fd < 0 || !self->drm_plane_id)
    return TRUE;

  ret = drmModeSetPlane (self->drm_fd, self->drm_plane_id, self->drm_crtc_id,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

  if (ret != 0) {
    GST_WARNING_OBJECT (self, "Failed to hide plane: %s", g_strerror (errno));
    return FALSE;
  }

  self->plane_visible = FALSE;
  return TRUE;
}
