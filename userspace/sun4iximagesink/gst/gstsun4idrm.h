/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 D.R.racer <drracer@seznam.cz> */
/*
 * gstsun4idrm.h — DRM helper declarations for sun4iximagesink
 */

#ifndef __GST_SUN4I_DRM_H__
#define __GST_SUN4I_DRM_H__

#include "gstsun4iximagesink.h"
#include <gst/allocators/gstdmabuf.h>

G_BEGIN_DECLS

gboolean  sun4i_drm_open           (GstSun4iXImageSink *self);
gboolean  sun4i_drm_find_resources (GstSun4iXImageSink *self);
gboolean  sun4i_drm_set_plane_zpos (GstSun4iXImageSink *self, guint32 plane_id,
                                    gint zpos);
guint32   sun4i_drm_import_dmabuf  (GstSun4iXImageSink *self, GstBuffer *buf);
gboolean  sun4i_drm_show_plane     (GstSun4iXImageSink *self, guint32 fb_id,
                                    gint dst_x, gint dst_y,
                                    gint dst_w, gint dst_h,
                                    gint src_x, gint src_y,
                                    gint src_w, gint src_h);
gboolean  sun4i_drm_hide_plane     (GstSun4iXImageSink *self);
void      sun4i_drm_close          (GstSun4iXImageSink *self);

G_END_DECLS

#endif /* __GST_SUN4I_DRM_H__ */
