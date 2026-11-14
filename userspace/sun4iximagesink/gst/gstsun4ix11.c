/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 D.R.racer <drracer@seznam.cz> */
/*
 * gstsun4ix11.c — X11 helper for sun4iximagesink
 *
 * Manages X11 window creation, external window support, and an event
 * thread that caches window geometry so show_frame needs no Xlib calls.
 */

#include <string.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <gst/video/videooverlay.h>

#include "gstsun4ix11.h"
#include "gstsun4idrm.h"

GST_DEBUG_CATEGORY_EXTERN (sun4i_ximagesink_debug);
#define GST_CAT_DEFAULT sun4i_ximagesink_debug

/* --- Open / Close --- */

gboolean
sun4i_x11_open (GstSun4iXImageSink *self)
{
  self->xdisplay = XOpenDisplay (self->display_name);
  if (!self->xdisplay) {
    GST_ERROR_OBJECT (self, "Failed to open X display '%s'",
        self->display_name ? self->display_name : "(default)");
    return FALSE;
  }

  self->screen_width = DisplayWidth (self->xdisplay,
      DefaultScreen (self->xdisplay));
  self->screen_height = DisplayHeight (self->xdisplay,
      DefaultScreen (self->xdisplay));

  GST_INFO_OBJECT (self, "X11 display opened: %s, screen %dx%d",
      DisplayString (self->xdisplay),
      self->screen_width, self->screen_height);

  return TRUE;
}

void
sun4i_x11_close (GstSun4iXImageSink *self)
{
  /* Stop event thread first */
  if (self->event_thread) {
    self->event_thread_run = FALSE;
    g_thread_join (self->event_thread);
    self->event_thread = NULL;
  }

  if (self->xdisplay) {
    g_mutex_lock (&self->x_lock);
    if (self->xwindow && !self->xwindow_external) {
      XDestroyWindow (self->xdisplay, self->xwindow);
    }
    self->xwindow = 0;
    XCloseDisplay (self->xdisplay);
    self->xdisplay = NULL;
    g_mutex_unlock (&self->x_lock);
  }

  self->xwindow_external = FALSE;
}

/*
 * Xlib is not thread-safe without XInitThreads(), which a plugin cannot call
 * early enough. Every use of self->xdisplay after start() therefore holds
 * x_lock (event thread, window creation in set_caps, set_window_handle).
 * Rendering never touches Xlib: it reads the geometry cached under win_lock.
 */

/* --- Window creation --- */

gboolean
sun4i_x11_create_window (GstSun4iXImageSink *self, gint width, gint height)
{
  Window root;
  XSetWindowAttributes attr;
  Atom wm_delete;

  if (!self->xdisplay)
    return FALSE;

  g_mutex_lock (&self->x_lock);
  root = DefaultRootWindow (self->xdisplay);

  attr.background_pixel = BlackPixel (self->xdisplay,
      DefaultScreen (self->xdisplay));
  attr.event_mask = StructureNotifyMask | VisibilityChangeMask |
      ExposureMask;

  self->xwindow = XCreateWindow (self->xdisplay, root,
      0, 0, width, height, 0,
      CopyFromParent, InputOutput, CopyFromParent,
      CWBackPixel | CWEventMask, &attr);

  if (!self->xwindow) {
    g_mutex_unlock (&self->x_lock);
    GST_ERROR_OBJECT (self, "Failed to create X11 window");
    return FALSE;
  }

  /* Set window title */
  XStoreName (self->xdisplay, self->xwindow, "sun4iximagesink");

  /* Handle WM_DELETE_WINDOW ourselves (see the event thread) */
  wm_delete = XInternAtom (self->xdisplay, "WM_DELETE_WINDOW", False);
  XSetWMProtocols (self->xdisplay, self->xwindow, &wm_delete, 1);

  XMapWindow (self->xdisplay, self->xwindow);
  XSync (self->xdisplay, False);
  self->xwindow_external = FALSE;
  g_mutex_unlock (&self->x_lock);

  /* Initialize cached geometry */
  g_mutex_lock (&self->win_lock);
  self->win_x = 0;
  self->win_y = 0;
  self->win_w = width;
  self->win_h = height;
  self->win_visible = TRUE;
  g_mutex_unlock (&self->win_lock);

  GST_INFO_OBJECT (self, "Created X11 window 0x%lx (%dx%d)",
      self->xwindow, width, height);

  return TRUE;
}

/*
 * An embedder may destroy its window at any time, and Xlib's default error
 * handler exits the process on BadWindow. Queries on the window therefore run
 * with a temporary handler that records the error instead. The handler is
 * process-global; it is installed only for the duration of the query.
 */
static gboolean x_error_seen;

static int
record_x_error (Display * dpy G_GNUC_UNUSED, XErrorEvent * ev G_GNUC_UNUSED)
{
  x_error_seen = TRUE;
  return 0;
}

/* Called with x_lock held: cache the window's absolute geometry. */
static void
update_geometry (GstSun4iXImageSink *self)
{
  XWindowAttributes wattr;
  Window child;
  gint abs_x, abs_y;
  int (*old_handler) (Display *, XErrorEvent *);
  gboolean ok;

  if (!self->xwindow)
    return;

  x_error_seen = FALSE;
  old_handler = XSetErrorHandler (record_x_error);
  ok = XGetWindowAttributes (self->xdisplay, self->xwindow, &wattr) &&
      XTranslateCoordinates (self->xdisplay, self->xwindow,
      DefaultRootWindow (self->xdisplay), 0, 0, &abs_x, &abs_y, &child);
  XSync (self->xdisplay, False);
  XSetErrorHandler (old_handler);
  if (!ok || x_error_seen)
    return;

  g_mutex_lock (&self->win_lock);
  self->win_x = abs_x;
  self->win_y = abs_y;
  self->win_w = wattr.width;
  self->win_h = wattr.height;
  self->win_visible = (wattr.map_state == IsViewable);
  g_mutex_unlock (&self->win_lock);

  GST_LOG_OBJECT (self, "geometry: abs=%d,%d size=%dx%d", abs_x, abs_y,
      wattr.width, wattr.height);
}

void
sun4i_x11_set_window_handle (GstSun4iXImageSink *self, guintptr xid)
{
  GST_DEBUG_OBJECT (self, "Using external window 0x%lx", (gulong) xid);

  if (!self->xdisplay) {
    /* Not started yet: picked up by the event thread setup in start(). */
    self->xwindow = (Window) xid;
    self->xwindow_external = xid != 0;
    return;
  }

  g_mutex_lock (&self->x_lock);
  /* If we already created our own window, destroy it */
  if (self->xwindow && !self->xwindow_external)
    XDestroyWindow (self->xdisplay, self->xwindow);

  self->xwindow = (Window) xid;
  self->xwindow_external = xid != 0;

  /* Select events on the external window; geometry from now on */
  if (self->xwindow) {
    XSelectInput (self->xdisplay, self->xwindow,
        StructureNotifyMask | VisibilityChangeMask | ExposureMask);
    XSync (self->xdisplay, False);
    update_geometry (self);
  }
  g_mutex_unlock (&self->x_lock);
}

/* --- Event thread --- */

/*
 * Check if window has _NET_WM_STATE_HIDDEN (minimized).
 */
static gboolean
check_window_hidden (Display *dpy, Window win)
{
  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after;
  unsigned char *data = NULL;
  Atom hidden_atom = XInternAtom (dpy, "_NET_WM_STATE_HIDDEN", True);
  Atom state_atom = XInternAtom (dpy, "_NET_WM_STATE", True);
  gboolean is_hidden = FALSE;

  int (*old_handler) (Display *, XErrorEvent *);
  int ret;

  if (hidden_atom == None || state_atom == None)
    return FALSE;

  /* The window may already be gone (see record_x_error). */
  x_error_seen = FALSE;
  old_handler = XSetErrorHandler (record_x_error);
  ret = XGetWindowProperty (dpy, win, state_atom, 0, 1024, False,
      XA_ATOM, &actual_type, &actual_format, &nitems, &bytes_after, &data);
  XSync (dpy, False);
  XSetErrorHandler (old_handler);

  if (ret == Success && !x_error_seen && data) {
    Atom *atoms = (Atom *) data;
    for (unsigned long i = 0; i < nitems; i++) {
      if (atoms[i] == hidden_atom) {
        is_hidden = TRUE;
        break;
      }
    }
  }
  if (data)
    XFree (data);

  return is_hidden;
}

/* The streaming thread may be calling SETPLANE: serialise with rendering. */
static void
hide_plane (GstSun4iXImageSink *self)
{
  g_mutex_lock (&self->render_lock);
  sun4i_drm_hide_plane (self);
  g_mutex_unlock (&self->render_lock);
}

enum
{
  ACTION_NONE,
  ACTION_RENDER,              /* geometry/visibility changed: re-show frame */
  ACTION_HIDE,
  ACTION_CLOSED,
};

/* Called with x_lock held. */
static int
handle_event (GstSun4iXImageSink *self, XEvent *xev, Atom wm_delete)
{
  switch (xev->type) {
    case ConfigureNotify:
      update_geometry (self);
      return ACTION_RENDER;

    case VisibilityNotify:{
      gboolean visible = (xev->xvisibility.state != VisibilityFullyObscured);

      g_mutex_lock (&self->win_lock);
      self->win_visible = visible;
      g_mutex_unlock (&self->win_lock);
      return visible ? ACTION_RENDER : ACTION_HIDE;
    }

    case UnmapNotify:
      g_mutex_lock (&self->win_lock);
      self->win_visible = FALSE;
      g_mutex_unlock (&self->win_lock);
      return ACTION_HIDE;

    case MapNotify:
      update_geometry (self);
      return ACTION_RENDER;

    case Expose:
      return ACTION_RENDER;

    case DestroyNotify:
      /* An embedding application destroyed its window. */
      if (xev->xdestroywindow.window == self->xwindow) {
        self->xwindow = 0;
        g_mutex_lock (&self->win_lock);
        self->win_visible = FALSE;
        g_mutex_unlock (&self->win_lock);
        return ACTION_HIDE;
      }
      return ACTION_NONE;

    case ClientMessage:
      /* Only our own window carries our WM_DELETE_WINDOW protocol. */
      if (!self->xwindow_external &&
          (Atom) xev->xclient.data.l[0] == wm_delete) {
        XUnmapWindow (self->xdisplay, self->xwindow);
        XFlush (self->xdisplay);
        return ACTION_CLOSED;
      }
      return ACTION_NONE;

    default:
      return ACTION_NONE;
  }
}

static gpointer
event_thread_func (gpointer data)
{
  GstSun4iXImageSink *self = GST_SUN4I_XIMAGESINK (data);
  Atom wm_delete;

  g_mutex_lock (&self->x_lock);
  wm_delete = XInternAtom (self->xdisplay, "WM_DELETE_WINDOW", False);
  g_mutex_unlock (&self->x_lock);

  GST_DEBUG_OBJECT (self, "Event thread started");

  while (self->event_thread_run) {
    gboolean changed = FALSE, closed = FALSE, visible;

    g_mutex_lock (&self->x_lock);
    while (XPending (self->xdisplay) > 0) {
      XEvent xev;

      XNextEvent (self->xdisplay, &xev);
      switch (handle_event (self, &xev, wm_delete)) {
        case ACTION_RENDER:
        case ACTION_HIDE:
          changed = TRUE;
          break;
        case ACTION_CLOSED:
          closed = TRUE;
          break;
        default:
          break;
      }
    }

    /* Also check for minimize via _NET_WM_STATE_HIDDEN, since some WMs
     * don't send VisibilityNotify for minimize */
    if (self->xwindow && check_window_hidden (self->xdisplay, self->xwindow)) {
      g_mutex_lock (&self->win_lock);
      if (self->win_visible) {
        self->win_visible = FALSE;
        changed = TRUE;
      }
      g_mutex_unlock (&self->win_lock);
    }
    g_mutex_unlock (&self->x_lock);

    /* Act on the state after the whole batch (e.g. Unmap then Map). */
    g_mutex_lock (&self->win_lock);
    visible = self->win_visible;
    g_mutex_unlock (&self->win_lock);

    /* Rendering takes render_lock only, never x_lock. */
    if (closed) {
      /* Like xvimagesink: stop showing frames and fail the pipeline. */
      GST_DEBUG_OBJECT (self, "WM_DELETE_WINDOW received");
      g_mutex_lock (&self->render_lock);
      self->window_closed = TRUE;
      sun4i_drm_hide_plane (self);
      g_mutex_unlock (&self->render_lock);
      GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
          ("Output window was closed"), (NULL));
    } else if (changed && !visible) {
      hide_plane (self);
    } else if (changed) {
      /* Follow moves/resizes also while paused (no new frames). */
      gst_video_overlay_expose (GST_VIDEO_OVERLAY (self));
    }

    g_usleep (10000); /* 10ms poll interval */
  }

  GST_DEBUG_OBJECT (self, "Event thread stopped");
  return NULL;
}

gboolean
sun4i_x11_start_event_thread (GstSun4iXImageSink *self)
{
  if (self->event_thread)
    return TRUE;

  /* A window handle set before start(): select its events now. */
  if (self->xwindow && self->xwindow_external)
    sun4i_x11_set_window_handle (self, self->xwindow);

  self->event_thread_run = TRUE;
  self->event_thread = g_thread_new ("sun4i-x11-events", event_thread_func,
      self);

  return (self->event_thread != NULL);
}

/* --- Get cached window rect --- */

void
sun4i_x11_get_window_rect (GstSun4iXImageSink *self,
    gint *x, gint *y, gint *w, gint *h, gboolean *visible)
{
  g_mutex_lock (&self->win_lock);
  *x = self->win_x;
  *y = self->win_y;
  *w = self->win_w;
  *h = self->win_h;
  *visible = self->win_visible;
  g_mutex_unlock (&self->win_lock);
}
