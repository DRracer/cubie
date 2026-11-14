/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 D.R.racer <drracer@seznam.cz> */
/*
 * sun4iximagesink — Zero-copy GStreamer video sink for Allwinner A20
 *
 * Displays hardware-decoded video (DMA-BUF from Cedrus) on a DRM overlay
 * plane while X11 is running. Uses zpos compositing (video on top).
 */

#ifndef __GST_SUN4I_XIMAGESINK_H__
#define __GST_SUN4I_XIMAGESINK_H__

#include <gst/gst.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video-info.h>
#include <gst/video/video-overlay-composition.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <X11/Xlib.h>

G_BEGIN_DECLS

#define GST_TYPE_SUN4I_XIMAGESINK (gst_sun4i_ximagesink_get_type())
G_DECLARE_FINAL_TYPE (GstSun4iXImageSink, gst_sun4i_ximagesink,
    GST, SUN4I_XIMAGESINK, GstVideoSink)

/* Allwinner tiled NV12 modifier (defined in drm_fourcc.h since ~5.14) */
#ifndef DRM_FORMAT_MOD_ALLWINNER_TILED
#define DRM_FORMAT_MOD_ALLWINNER_TILED  (((__u64)0x09 << 56) | 1)
#endif

struct _GstSun4iXImageSink
{
  GstVideoSink parent;

  /* --- Properties --- */
  gchar *display_name;      /* X11 display name (NULL = default) */
  gint connector_id;        /* DRM connector ID (0 = auto) */
  gint plane_id;            /* DRM plane ID (0 = auto) */
  gint zpos;                /* overlay plane zpos (default 2) */

  /* --- DRM state --- */
  int drm_fd;
  struct _Sun4iDrmDev *drm_dev;  /* refcounted owner of drm_fd (FB cache) */
  guint32 drm_crtc_id;
  guint32 drm_plane_id;
  guint32 drm_connector_id;
  guint32 hdisplay;         /* CRTC mode width */
  guint32 vdisplay;         /* CRTC mode height */
  gboolean has_modifiers;   /* DRM_CAP_ADDFB2_MODIFIERS */
  gboolean plane_visible;   /* is the overlay plane currently on? */

  /* --- X11 state --- */
  Display *xdisplay;
  Window xwindow;
  gboolean xwindow_external; /* TRUE if set via GstVideoOverlay */
  GThread *event_thread;
  gboolean event_thread_run;

  /* cached window geometry from event thread */
  GMutex win_lock;
  GMutex x_lock;            /* serialises all Xlib use after start() */
  gint win_x, win_y, win_w, win_h;
  gboolean win_visible;
  gboolean window_closed;   /* user closed our window (WM_DELETE_WINDOW) */

  /* --- Caps / video info --- */
  GstVideoInfo vinfo;
  guint32 drm_fourcc;
  guint64 drm_modifier;

  /* --- Render state --- */
  /* Serialises rendering between the streaming thread and expose (X event
   * thread); protects last_buffer and the plane state. */
  GMutex render_lock;
  GstBuffer *last_buffer;   /* stashed for expose redraw */

  /* --- GstVideoOverlay render rectangle --- */
  gboolean have_render_rect;
  gint render_x, render_y, render_w, render_h;

  /* screen dimensions for clamping */
  gint screen_width, screen_height;
};

G_END_DECLS

#endif /* __GST_SUN4I_XIMAGESINK_H__ */
