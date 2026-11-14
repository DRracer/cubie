# GStreamer patches

| Patch | Plugin | What |
|---|---|---|
| `0001-v4l2-map-V4L2_PIX_FMT_NV12_32L32-to-GST_VIDEO_FORMAT.patch` | gst-plugins-good, `v4l2` (1.26.2) | lets the stateful JPEG decoder node (`cedrus-jpeg`, kernel patch 0035) register as `v4l2jpegdec`: without a mapping for its tiled 4:2:0 output it is skipped. 4:2:0 only (the 4:2:2/4:4:4 output format is driver-private) |
| `0002-v4l2codecs-map-sunxi-tiled-nv12-to-drm.patch` | gst-plugins-bad, `v4l2codecs` (1.26) | maps the stateless decoders' tiled NV12 to `DRM_FORMAT_NV12` + `DRM_FORMAT_MOD_ALLWINNER_TILED`, so decoded frames can go to a DMA_DRM sink (e.g. a display plane) without a CPU copy |

Both are small enough to carry in a Debian source package (`debian/patches/`) or to apply to a
GStreamer checkout. Neither has been submitted upstream yet.
