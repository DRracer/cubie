/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 D.R.racer <drracer@seznam.cz> */
/*
 * gstsun4iximagesink.c — Zero-copy GStreamer video sink for Allwinner A20
 *
 * Displays DMA-BUF video on a DRM overlay plane while X11 is running.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/video/videooverlay.h>
#include <gst/video/video-info-dma.h>

#include "gstsun4iximagesink.h"
#include "gstsun4idrm.h"
#include "gstsun4ix11.h"

GST_DEBUG_CATEGORY (sun4i_ximagesink_debug);
#define GST_CAT_DEFAULT sun4i_ximagesink_debug

/* --- Properties --- */
enum
{
  PROP_0,
  PROP_DISPLAY,
  PROP_CONNECTOR_ID,
  PROP_PLANE_ID,
  PROP_ZPOS,
};

#define DEFAULT_ZPOS 2

/* --- Pad template --- */
/* Accept DMA_DRM caps with Allwinner tiled NV12 or plain NV12 */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
        "video/x-raw(memory:DMABuf), format=DMA_DRM, "
        "drm-format=(string){ NV12:0x0900000000000001, NV12 }"
    )
);

/* Renders @buf; called with render_lock held (show_frame and expose) */
static GstFlowReturn
gst_sun4i_ximagesink_render_locked (GstSun4iXImageSink *self, GstBuffer *buf);

/* --- GstVideoOverlay interface --- */

static void
gst_sun4i_ximagesink_set_window_handle (GstVideoOverlay *overlay,
    guintptr handle)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (overlay);

  GST_DEBUG_OBJECT (self, "set_window_handle: 0x%lx", (gulong) handle);
  sun4i_x11_set_window_handle (self, handle);
}

static void
gst_sun4i_ximagesink_expose (GstVideoOverlay *overlay)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (overlay);

  GST_DEBUG_OBJECT (self, "expose");
  g_mutex_lock (&self->render_lock);
  if (self->last_buffer)
    gst_sun4i_ximagesink_render_locked (self, self->last_buffer);
  g_mutex_unlock (&self->render_lock);
}

static void
gst_sun4i_ximagesink_set_render_rectangle (GstVideoOverlay *overlay,
    gint x, gint y, gint w, gint h)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (overlay);

  GST_DEBUG_OBJECT (self, "set_render_rectangle: %d,%d %dx%d", x, y, w, h);
  self->have_render_rect = TRUE;
  self->render_x = x;
  self->render_y = y;
  self->render_w = w;
  self->render_h = h;
}

static void
gst_sun4i_ximagesink_video_overlay_init (GstVideoOverlayInterface *iface)
{
  iface->set_window_handle = gst_sun4i_ximagesink_set_window_handle;
  iface->expose = gst_sun4i_ximagesink_expose;
  iface->set_render_rectangle = gst_sun4i_ximagesink_set_render_rectangle;
}

/* --- Type definition with GstVideoOverlay interface --- */
G_DEFINE_TYPE_WITH_CODE (GstSun4iXImageSink, gst_sun4i_ximagesink,
    GST_TYPE_VIDEO_SINK,
    G_IMPLEMENT_INTERFACE (GST_TYPE_VIDEO_OVERLAY,
        gst_sun4i_ximagesink_video_overlay_init));

/* --- Letterbox / clamping helpers --- */

/*
 * Calculate destination rect preserving aspect ratio within the target area.
 */
static void
calc_letterbox (gint video_w, gint video_h,
    gint target_w, gint target_h,
    gint *out_x, gint *out_y, gint *out_w, gint *out_h)
{
  gdouble aspect_video = (gdouble) video_w / video_h;
  gdouble aspect_target = (gdouble) target_w / target_h;

  if (aspect_video > aspect_target) {
    /* pillarbox — video wider than target */
    *out_w = target_w;
    *out_h = (gint) (target_w / aspect_video + 0.5);
    *out_x = 0;
    *out_y = (target_h - *out_h) / 2;
  } else {
    /* letterbox — video taller than target */
    *out_h = target_h;
    *out_w = (gint) (target_h * aspect_video + 0.5);
    *out_x = (target_w - *out_w) / 2;
    *out_y = 0;
  }
}

/*
 * Clamp dest rect to screen boundaries, adjusting source rect proportionally.
 * src coordinates are in 16.16 fixed point for drmModeSetPlane.
 */
static gboolean
clamp_to_screen (GstSun4iXImageSink *self,
    gint *dst_x, gint *dst_y, gint *dst_w, gint *dst_h,
    gint *src_x, gint *src_y, gint *src_w, gint *src_h)
{
  gint scr_w = self->hdisplay;
  gint scr_h = self->vdisplay;
  gint orig_dst_w = *dst_w;
  gint orig_dst_h = *dst_h;

  /* fully off-screen? */
  if (*dst_x + *dst_w <= 0 || *dst_y + *dst_h <= 0 ||
      *dst_x >= scr_w || *dst_y >= scr_h)
    return FALSE;

  /* clamp left edge */
  if (*dst_x < 0) {
    gint clip = -*dst_x;
    *src_x += (gint64) clip * *src_w / orig_dst_w;
    *src_w -= (gint64) clip * *src_w / orig_dst_w;
    *dst_w -= clip;
    *dst_x = 0;
  }
  /* clamp top edge */
  if (*dst_y < 0) {
    gint clip = -*dst_y;
    *src_y += (gint64) clip * *src_h / orig_dst_h;
    *src_h -= (gint64) clip * *src_h / orig_dst_h;
    *dst_h -= clip;
    *dst_y = 0;
  }
  /* clamp right edge */
  if (*dst_x + *dst_w > scr_w) {
    gint clip = (*dst_x + *dst_w) - scr_w;
    *src_w -= (gint64) clip * *src_w / orig_dst_w;
    *dst_w -= clip;
  }
  /* clamp bottom edge */
  if (*dst_y + *dst_h > scr_h) {
    gint clip = (*dst_y + *dst_h) - scr_h;
    *src_h -= (gint64) clip * *src_h / orig_dst_h;
    *dst_h -= clip;
  }

  return (*dst_w > 0 && *dst_h > 0);
}

/* --- GstVideoSink virtual methods --- */

static gboolean
gst_sun4i_ximagesink_start (GstBaseSink *bsink)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (bsink);

  GST_DEBUG_OBJECT (self, "start");

  self->window_closed = FALSE;

  /* X first: the DRM fd is authenticated through the X server. */
  if (!sun4i_x11_open (self))
    return FALSE;

  if (!sun4i_drm_open (self))
    goto fail_x11;

  if (!sun4i_drm_find_resources (self))
    goto fail_drm;

  /* Events for our own window (created in set_caps) or an embedder's. */
  if (!sun4i_x11_start_event_thread (self))
    goto fail_drm;

  return TRUE;

fail_drm:
  sun4i_drm_close (self);
fail_x11:
  sun4i_x11_close (self);
  return FALSE;
}

static gboolean
gst_sun4i_ximagesink_stop (GstBaseSink *bsink)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (bsink);

  GST_DEBUG_OBJECT (self, "stop");

  /* The X event thread (expose) runs until sun4i_x11_close(). */
  g_mutex_lock (&self->render_lock);
  sun4i_drm_hide_plane (self);
  gst_clear_buffer (&self->last_buffer);
  g_mutex_unlock (&self->render_lock);
  sun4i_x11_close (self);
  sun4i_drm_close (self);

  return TRUE;
}

static gboolean
gst_sun4i_ximagesink_set_caps (GstBaseSink *bsink, GstCaps *caps)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (bsink);
  GstVideoInfoDmaDrm drm_info;

  GST_DEBUG_OBJECT (self, "set_caps: %" GST_PTR_FORMAT, caps);

  if (!gst_video_info_dma_drm_from_caps (&drm_info, caps)) {
    GST_ERROR_OBJECT (self, "Failed to parse DMA-DRM caps");
    return FALSE;
  }

  self->drm_fourcc = drm_info.drm_fourcc;
  self->drm_modifier = drm_info.drm_modifier;
  self->vinfo = drm_info.vinfo;

  GST_INFO_OBJECT (self, "Video: %dx%d, drm_fourcc=0x%08x, modifier=0x%016"
      G_GINT64_MODIFIER "x",
      GST_VIDEO_INFO_WIDTH (&self->vinfo),
      GST_VIDEO_INFO_HEIGHT (&self->vinfo),
      self->drm_fourcc, self->drm_modifier);

  /* Give an embedding application the chance to provide its window. */
  if (!self->xwindow)
    gst_video_overlay_prepare_window_handle (GST_VIDEO_OVERLAY (self));

  /* Create or resize the X11 window */
  if (!self->xwindow_external && !self->xwindow) {
    if (!sun4i_x11_create_window (self,
            GST_VIDEO_INFO_WIDTH (&self->vinfo),
            GST_VIDEO_INFO_HEIGHT (&self->vinfo)))
      return FALSE;
  }

  /*
   * zpos is not settable by a non-master client (OBJ_SETPROPERTY stays
   * master-only); sun4i overlay planes already sit above the primary.
   */

  return TRUE;
}

static GstFlowReturn
gst_sun4i_ximagesink_show_frame (GstVideoSink *vsink, GstBuffer *buf)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (vsink);
  GstFlowReturn ret;

  g_mutex_lock (&self->render_lock);
  ret = gst_sun4i_ximagesink_render_locked (self, buf);
  g_mutex_unlock (&self->render_lock);

  return ret;
}

static GstFlowReturn
gst_sun4i_ximagesink_render_locked (GstSun4iXImageSink *self, GstBuffer *buf)
{
  gint win_x, win_y, win_w, win_h;
  gboolean visible;
  guint32 fb_id;
  gint video_w, video_h;
  gint lb_x, lb_y, lb_w, lb_h;
  gint dst_x, dst_y, dst_w, dst_h;
  gint src_x, src_y, src_w, src_h;

  if (self->window_closed)
    return GST_FLOW_ERROR;

  /* 1. Get cached window geometry */
  sun4i_x11_get_window_rect (self, &win_x, &win_y, &win_w, &win_h, &visible);

  /* 2. If not visible, hide the DRM plane */
  if (!visible || win_w <= 0 || win_h <= 0) {
    if (self->plane_visible)
      sun4i_drm_hide_plane (self);
    gst_buffer_replace (&self->last_buffer, buf);
    return GST_FLOW_OK;
  }

  /* 3. Import DMA-BUF → fb_id (cached on GstMemory) */
  fb_id = sun4i_drm_import_dmabuf (self, buf);
  if (fb_id == 0) {
    GST_ERROR_OBJECT (self, "Failed to import DMA-BUF as DRM framebuffer");
    return GST_FLOW_ERROR;
  }

  /* 4. Calculate letterboxed destination within window */
  video_w = GST_VIDEO_INFO_WIDTH (&self->vinfo);
  video_h = GST_VIDEO_INFO_HEIGHT (&self->vinfo);

  if (self->have_render_rect) {
    calc_letterbox (video_w, video_h,
        self->render_w, self->render_h,
        &lb_x, &lb_y, &lb_w, &lb_h);
    lb_x += self->render_x;
    lb_y += self->render_y;
  } else {
    calc_letterbox (video_w, video_h, win_w, win_h,
        &lb_x, &lb_y, &lb_w, &lb_h);
  }

  /* Convert to screen coordinates */
  dst_x = win_x + lb_x;
  dst_y = win_y + lb_y;
  dst_w = lb_w;
  dst_h = lb_h;

  /* Source rect covers the full video in 16.16 fixed point */
  src_x = 0;
  src_y = 0;
  src_w = video_w << 16;
  src_h = video_h << 16;

  /* 5. Clamp to screen edges */
  if (!clamp_to_screen (self, &dst_x, &dst_y, &dst_w, &dst_h,
          &src_x, &src_y, &src_w, &src_h)) {
    /* entirely off screen */
    if (self->plane_visible)
      sun4i_drm_hide_plane (self);
    gst_buffer_replace (&self->last_buffer, buf);
    return GST_FLOW_OK;
  }

  /* 6. drmModeSetPlane */
  if (!sun4i_drm_show_plane (self, fb_id,
          dst_x, dst_y, dst_w, dst_h,
          src_x, src_y, src_w, src_h)) {
    GST_ERROR_OBJECT (self, "drmModeSetPlane failed");
    return GST_FLOW_ERROR;
  }

  /* 7. Stash buffer for expose redraw */
  gst_buffer_replace (&self->last_buffer, buf);

  return GST_FLOW_OK;
}

static gboolean
gst_sun4i_ximagesink_propose_allocation (GstBaseSink *bsink, GstQuery *query)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (bsink);

  GST_DEBUG_OBJECT (self, "propose_allocation");

  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  return TRUE;
}

/* --- GObject methods --- */

static void
gst_sun4i_ximagesink_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (object);

  switch (prop_id) {
    case PROP_DISPLAY:
      g_free (self->display_name);
      self->display_name = g_value_dup_string (value);
      break;
    case PROP_CONNECTOR_ID:
      self->connector_id = g_value_get_int (value);
      break;
    case PROP_PLANE_ID:
      self->plane_id = g_value_get_int (value);
      break;
    case PROP_ZPOS:
      self->zpos = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_sun4i_ximagesink_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (object);

  switch (prop_id) {
    case PROP_DISPLAY:
      g_value_set_string (value, self->display_name);
      break;
    case PROP_CONNECTOR_ID:
      g_value_set_int (value, self->connector_id);
      break;
    case PROP_PLANE_ID:
      g_value_set_int (value, self->plane_id);
      break;
    case PROP_ZPOS:
      g_value_set_int (value, self->zpos);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_sun4i_ximagesink_finalize (GObject *object)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (object);

  g_free (self->display_name);
  g_mutex_clear (&self->win_lock);
  g_mutex_clear (&self->render_lock);
  g_mutex_clear (&self->x_lock);
  gst_clear_buffer (&self->last_buffer);

  G_OBJECT_CLASS (gst_sun4i_ximagesink_parent_class)->finalize (object);
}

static void
gst_sun4i_ximagesink_init (GstSun4iXImageSink *self)
{
  self->display_name = NULL;
  self->connector_id = 0;
  self->plane_id = 0;
  self->zpos = DEFAULT_ZPOS;
  self->drm_fd = -1;
  self->xdisplay = NULL;
  self->xwindow = 0;
  self->xwindow_external = FALSE;
  self->event_thread = NULL;
  self->event_thread_run = FALSE;
  self->last_buffer = NULL;
  self->window_closed = FALSE;
  self->have_render_rect = FALSE;
  self->plane_visible = FALSE;
  g_mutex_init (&self->win_lock);
  g_mutex_init (&self->render_lock);
  g_mutex_init (&self->x_lock);
}

static void
gst_sun4i_ximagesink_class_init (GstSun4iXImageSinkClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *basesink_class = GST_BASE_SINK_CLASS (klass);
  GstVideoSinkClass *videosink_class = GST_VIDEO_SINK_CLASS (klass);

  gobject_class->set_property = gst_sun4i_ximagesink_set_property;
  gobject_class->get_property = gst_sun4i_ximagesink_get_property;
  gobject_class->finalize = gst_sun4i_ximagesink_finalize;

  g_object_class_install_property (gobject_class, PROP_DISPLAY,
      g_param_spec_string ("display", "Display",
          "X11 display name", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CONNECTOR_ID,
      g_param_spec_int ("connector-id", "Connector ID",
          "DRM connector ID (0 = auto)", 0, G_MAXINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PLANE_ID,
      g_param_spec_int ("plane-id", "Plane ID",
          "DRM plane ID (0 = auto)", 0, G_MAXINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ZPOS,
      g_param_spec_int ("zpos", "Z Position",
          "Overlay plane zpos (higher = on top)", 0, 15, DEFAULT_ZPOS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (element_class,
      "Sun4i X11 DRM overlay video sink",
      "Sink/Video",
      "Zero-copy video display via DRM overlay plane under X11 "
      "(Allwinner A20 sun4i-drm)",
      "drracer");

  gst_element_class_add_static_pad_template (element_class, &sink_template);

  basesink_class->start = gst_sun4i_ximagesink_start;
  basesink_class->stop = gst_sun4i_ximagesink_stop;
  basesink_class->set_caps = gst_sun4i_ximagesink_set_caps;
  basesink_class->propose_allocation = gst_sun4i_ximagesink_propose_allocation;

  videosink_class->show_frame = gst_sun4i_ximagesink_show_frame;
}

/* --- Plugin registration --- */

static gboolean
plugin_init (GstPlugin *plugin)
{
  GST_DEBUG_CATEGORY_INIT (sun4i_ximagesink_debug,
      "sun4iximagesink", 0, "Sun4i X11 DRM overlay sink");

  return gst_element_register (plugin, "sun4iximagesink",
      GST_RANK_PRIMARY, GST_TYPE_SUN4I_XIMAGESINK);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    sun4iximagesink,
    "Sun4i X11 DRM overlay video sink for Allwinner A20",
    plugin_init,
    "0.1.0",
    "LGPL",
    "sun4iximagesink",
    "https://github.com/DRracer/cubie"
)
