# sun4iximagesink: zero-copy GStreamer video sink for X11

A GStreamer video sink for the A10/A20 that shows hardware-decoded video on a display overlay
plane while X is running: the decoder's tiled NV12 buffers go to the plane as dma-bufs, without a
CPU copy. The plane follows the X window (position, size, closing); an X window can also be given
to it through `GstVideoOverlay` (embedding in a player).

Needs:
- kernel patches 0001 and 0002 (an authenticated X client may use the overlay planes; see the
  security note in [`../../kernel/sunxi-6.18/README.md`](../../kernel/sunxi-6.18/README.md)) and the
  video engine patches 0023-0030;
- `0002-v4l2codecs-map-sunxi-tiled-nv12-to-drm.patch` from [`../gstreamer/`](../gstreamer/) in
  gst-plugins-bad, so that `v4l2slh264dec` exports its buffers with the tiled DRM modifier.

Build and install on the board:

```sh
sudo apt install meson ninja-build libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
                 libdrm-dev libx11-dev libx11-xcb-dev libxcb-dri2-0-dev
meson setup build --prefix=/usr --libdir=lib/arm-linux-gnueabihf
ninja -C build
sudo ninja -C build install
```

Play:

```sh
gst-launch-1.0 filesrc location=clip.mp4 ! qtdemux ! h264parse ! v4l2slh264dec ! sun4iximagesink
gst-play-1.0 --flags=0x57 clip.mp4      # playbin with native-video, see below
```

The sink has rank primary, so `playbin3` picks it. Use `flags=native-video` (0x57): with the
default flags playbin inserts `deinterlace`, `videobalance` and `videoconvert`, which pull the
frames into system memory and de-tile them on the CPU (many dropped frames at 1080p). With
native-video the chain is zero-copy; 1080p25 H.264 played without drops on the A20.

For players, the VDPAU driver ([`../vdpau-sun4i/`](../vdpau-sun4i/)) is the more complete path
(mpv, MPlayer); this sink is for GStreamer pipelines.

Licence: LGPL-2.1-or-later, like GStreamer (see `COPYING`). The design follows the idea of
Rockchip's `rkximagesink` (an X11 window with the video on a DRM plane).
