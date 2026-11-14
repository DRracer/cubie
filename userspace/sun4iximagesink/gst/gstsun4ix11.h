/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 D.R.racer <drracer@seznam.cz> */
/*
 * gstsun4ix11.h — X11 helper declarations for sun4iximagesink
 */

#ifndef __GST_SUN4I_X11_H__
#define __GST_SUN4I_X11_H__

#include "gstsun4iximagesink.h"

G_BEGIN_DECLS

gboolean  sun4i_x11_open               (GstSun4iXImageSink *self);
gboolean  sun4i_x11_create_window      (GstSun4iXImageSink *self,
                                         gint width, gint height);
void      sun4i_x11_set_window_handle   (GstSun4iXImageSink *self, guintptr xid);
gboolean  sun4i_x11_start_event_thread  (GstSun4iXImageSink *self);
void      sun4i_x11_get_window_rect     (GstSun4iXImageSink *self,
                                         gint *x, gint *y,
                                         gint *w, gint *h,
                                         gboolean *visible);
void      sun4i_x11_close               (GstSun4iXImageSink *self);

G_END_DECLS

#endif /* __GST_SUN4I_X11_H__ */
