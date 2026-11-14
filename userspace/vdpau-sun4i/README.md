# libvdpau_sun4i-drm: zero-copy H.264 playback under X on the A20

A VDPAU driver for mainline kernels on the Allwinner A20: it decodes H.264 with the video engine
(cedrus, V4L2 stateless decoder) and shows the decoded pictures directly on an overlay plane of
the display (sun4i-drm), scaled by the display frontend. The picture never passes through the
CPU or the GPU. Stock players work with it: `mpv --vo=vdpau --hwdec=vdpau` and
`mplayer -vo vdpau -vc ffh264vdpau`. `ffmpeg -hwaccel vdpau` can decode through it as well
(the driver reads the pictures back for that).

It plays the role that libvdpau-sunxi (Jens Kuske) played on the vendor 3.4 kernel, whose
structure it follows; the code was written for the mainline interfaces (V4L2 requests, DRM
planes).

Measured: 1080p25 H.264 in mpv, windowed, about 27 % CPU on a Cubietruck (0 dropped frames
and 17-20 % CPU in an earlier measurement on a Cubieboard2; about 40 % while the on-screen
controller is shown). Video on either monitor of a dual-head desktop, and a window across both
monitors shows on both. Tested with Debian Trixie's mpv 0.40 and FFmpeg 7.1.

## Requirements

- Kernel patches 0001 and 0002 (they let the player use the overlay planes while the X server
  is the display master), 0023-0029 (the video engine driver; without 0028/0029 decoding is slow
  and B-frames come out wrong), and for best results 0004, 0013, 0016 and 0017 (crops, video on
  the second head, scaling quality and limits). See `../../kernel/sunxi-6.18/README.md`.
- The `sun4i_backend.client_planes` parameter of 0002 at its default (0xa: overlay layer 1 for
  the video, layer 3 for the on-screen display).
- X11 with the `modesetting` driver (Wayland is not supported).

## Build and install (on the board)

```sh
sudo apt install build-essential pkg-config libvdpau-dev libx11-dev libx11-xcb-dev \
                 libxcb1-dev libxcb-dri2-0-dev libdrm-dev
make
sudo make install    # /usr/lib/arm-linux-gnueabihf/vdpau/libvdpau_sun4i-drm.so.1 (+ .so link)
```

The Makefile builds for the Cortex-A7 with NEON (`-mcpu=cortex-a7 -mfpu=neon-vfpv4`), i.e. for
the A20. For the A10 (Cortex-A8) build with `make CPUFLAGS="-mcpu=cortex-a8 -mfpu=neon"`.

libvdpau normally selects the driver by itself (the X server reports `sun4i-drm` as its DRI2
driver name). If a player reports that it cannot load a VDPAU driver, set
`VDPAU_DRIVER=sun4i-drm` in its environment (e.g. in `/etc/environment`).

Play:

```sh
mpv --vo=vdpau --hwdec=vdpau video.mp4
mplayer -vo vdpau -vc ffh264vdpau video.mp4
```

To make it mpv's default, put `vo=vdpau` and `hwdec=vdpau` into `~/.config/mpv/mpv.conf`. Both
are needed; with only one of them the zero-copy overlay path is not used.

Environment variables: `VDPAU_SUN4I_DEBUG=1` prints what the driver does to stderr;
`VDPAU_SUN4I_PIPELINE=1` is an experiment (one picture in flight while the next is queued), off
by default.

## Checking the decoder: vdpau-test.sh and vdpau-frames.sh

`vdpau-test.sh clip...` decodes each clip with FFmpeg through this driver and with FFmpeg's
software decoder and compares every frame (`RESULT <clip> PASS frames=N` when all frames are
bit-exact). `vdpau-frames.sh clip` prints, per frame in display order, which frames differ, plus
the PSNR. They need `ffmpeg` and a running X display. By default they load the driver from
their own directory (run them from the build directory, or set `VDPAU_DRIVER_PATH`).

Results with the kernel of this repository (A20): Big Buck Bunny 1080p (300 frames) and a 1080p25 TV clip
(1547 frames) bit-exact. Synthetic clips (x264 `testsrc2`) can show a few chroma values differing
by 1-2 at the bottom edge (a kernel/hardware decoder issue, same with GStreamer, not this driver).

## Known limits

- **H.264 only** (baseline, main, high; 8-bit 4:2:0). Field-coded pictures (PAFF, common in
  interlaced TV recordings) are not supported: the driver reports an error for them (how each
  player reacts was not tested). MBAFF streams are passed to the hardware but were not tested.
  No MPEG-2 through VDPAU. Tested with progressive content.
- **The video is always on top.** The overlay plane is above the X desktop and there is no
  colour key yet: menus or windows over the video window are hidden behind the video.
- **On-screen display:** mpv's OSD/controller is blended in software into a second overlay
  plane; while it is visible (e.g. the mouse over the video), playback costs more CPU and can drop
  frames. `osc=no` in `mpv.conf` avoids it. When an OSD surface is re-rendered while visible, a
  short tear can show.
- **Downscaling:** the display frontend scales video down to about 2x vertically at full
  quality; beyond that, kernel patch 0017 skips source lines, and planes the frontend cannot
  fetch fast enough (roughly beyond 4x for a 1080p video on a 1080p60 head) are refused: the video
  window then stays empty (shows the desktop colour). Make the window bigger.
  The driver prints a one-time warning `... disturb the desktop beside the window` for
  vertical downscaling beyond 2x; with kernel patch 0017 that warning is obsolete (the kernel
  now refuses what it cannot show instead of disturbing the desktop).
- `vdpauinfo` aborts on a query the driver does not implement (cosmetic).

## Licence

MIT, see `LICENSE` (and `../../LICENSES.md` for the relation to libvdpau-sunxi).
