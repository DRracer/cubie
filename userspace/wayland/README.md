# Wayland: zero-copy video on Weston (experimental)

Research on an MK802 (A10) with Debian Trixie: can hardware-decoded video reach a display plane
without a copy under Wayland, as it does under X with the VDPAU driver? Result: **yes on a patched
Weston, no on wlroots compositors** (labwc, so no LXQt on Wayland). The desktop on these boards
stays Xorg; these patches are kept for later.

| Session | Zero-copy VE video | glmark2 (1080p60) |
|---|---|---|
| Xorg + glamor + 2D engine (reference) | yes (VDPAU, overlay plane) | 94 |
| Weston 14 + `weston/` patches | **yes**: 1080p25 H.264, 0 dropped frames, about 26 % CPU of the single Cortex-A8 | 93 |
| labwc 0.8.3 (wlroots 0.18) | no: CPU de-tiling (about 92 % CPU, drops), or the video stays invisible | 66 |

Why: the video engine decodes only into NV12 with `DRM_FORMAT_MOD_ALLWINNER_TILED`. The sun4i
display planes scan that out directly, but lima/Mesa can neither import nor texture it. wlroots
0.18 assumes every client buffer is texturable, so such a buffer reaches neither the GPU
composition nor a plane. Weston can put it on a plane once the patches below let it through, but
it has no wlr-layer-shell, so LXQt's panel and desktop cannot run on it.

## Patches

`weston/`, against Weston 14.0.2 (Debian 14.0.2-1 source):
1. `backend-drm: import dmabufs into KMS directly when GBM refuses them`: `gbm_bo_import()` fails
   for the tiled modifier (lima knows only LINEAR and ARM 16x16), although
   `drmPrimeFDToHandle()` + `drmModeAddFB2WithModifiers()` work.
2. `linux-dmabuf: accept scan-out-only dmabufs without direct-display`.
3. `dmabuf-feedback: offer scan-out-only formats in scanout tranches` (the tranche is the union of
   the plane formats instead of planes intersected with the renderer formats).
4. `linux-dmabuf: advertise scan-out-only modifiers to version 3 clients` (waylandsink 1.26 does
   not use dmabuf feedback).

`gst-wayland/`, against `gst-libs/gst/wayland` of gst-plugins-bad 1.26.2:
- **0002 `wayland: convert tiled strides of DMA_DRM buffers to bytes`**: a real bug, upstream
  candidate. waylandsink passed the V4L2 tile stride as the plane stride, which Weston rejects
  ("invalid buffer stride or height for plane 0").
- 0001 `GST_WL_EXTRA_DMABUF` and 0003 `GST_WL_DISPLAY_HEIGHT`: experimental hooks used for the
  labwc tests, not meant for upstream; they do nothing while the variables are unset.

Nothing has been submitted upstream. The Weston patches would need rebasing onto Weston main
(its format and feedback code has changed) and a discussion about scan-out-only buffers.

## Use

Build the patched Weston and libgstwayland into a separate prefix (the system packages stay
untouched), start the patched Weston as a session, then play from a terminal inside it:

```sh
LD_LIBRARY_PATH=/opt/gst-patched/lib gst-launch-1.0 filesrc location=clip.mp4 ! qtdemux ! h264parse ! \
    v4l2slh264dec ! queue ! waylandsink
```

The `queue` matters on the single-core A10. `/opt/gst-patched` stands for wherever you installed
the patched `libgstwayland-1.0.so.0`.
