# cubie: mainline Linux desktop and video for Allwinner A10/A20 boards

Kernel patches, device-tree overlays, a patched X server, a VDPAU video driver and build notes
that turn a current Armbian image (Linux 6.18) on an A20 Cubietruck or an A10 MK802 into a usable
desktop: working GPU, VGA and dual head, a 2D engine, zero-copy hardware video playback, and
hardware H.264 and JPEG encoding and decoding.

This is an umbrella repository: the patches live here until they are merged into Armbian or the
mainline kernel. Everything was tested on a Cubietruck (A20, 2 GiB) and a Rikomagic MK802 (A10,
1 GiB) running Armbian Trixie with kernel 6.18.56; parts were first tested on a Cubieboard2 (A20,
1 GiB). Other A10/A20 boards are untested; the kernel patches are written for the A10/A20 in
general, the overlays and Bluetooth notes are board specific.

> Status: hobby work, no warranty. Until the patches are merged you need a self-built kernel
> ([`armbian/README.md`](armbian/README.md)).

## What works

| Feature | Stock mainline 6.18 | With this repository | Patches |
|---|---|---|---|
| GPU (Mali-400, lima, Mesa) | **broken on 2 GiB boards** (black screen, `gpmmu page fault`); rare GPU hangs on all boards | works: Xorg/glamor, SDDM, LXQt, kmscube, glmark2; the hang was root-caused and fixed on the A10, the A20 fault is very likely the same bug | 0005, 0009, 0042-0046 |
| VGA (Cubietruck) | not supported | works, EDID over DDC, hot-plug by polling | 0010-0012 |
| HDMI + VGA dual head | - | works (1080p + 1280x1024 tested) | 0010-0013 |
| Hardware cursor | software cursor | 32x32 cursor plane on each head | 0014 |
| Async page flips | no | fullscreen GL without vsync flips instead of copying (1080p: 29 -> 108 FPS; the DRAM clock also differed between the runs, see the Cubietruck guide) | 0015 |
| 2D engine (G2D, "mixer processor") | no driver | driver `sun4i-mp`; the patched X server copies with it (scrolling ~4x faster) | 0018-0022, 0038 + [`userspace/xserver-g2d/`](userspace/xserver-g2d/) |
| Video overlay scaling | aliasing; >2x vertical downscale corrupts the screen | filtered; skips lines or refuses cleanly beyond the frontend's throughput | 0004, 0016, 0017 |
| H.264 / MPEG-2 hardware decode (cedrus) | half clock after boot; B-frames wrong on ARM32 (cache alias); no X player path | bit-exact on real content, VE at 192 MHz; zero-copy playback in mpv/MPlayer via VDPAU (or GStreamer via `sun4iximagesink`) on an overlay plane | 0023-0030, 0001, 0002 + [`userspace/vdpau-sun4i/`](userspace/vdpau-sun4i/) (**security:** 0001/0002 let any X client draw over the whole screen, lock screen included; see the [patch index](kernel/sunxi-6.18/README.md)) |
| H.264 hardware encode | no encoder | V4L2 M2M encoder up to 1920x1088, CBR rate control (FFmpeg `h264_v4l2m2m`), per-MB motion vectors, encode and decode at once; A10 output bit-identical to the A20 | 0023, 0024, 0031, 0032 |
| (M)JPEG hardware decode / encode | no | stateful decoder `cedrus-jpeg` (e.g. UVC webcam MJPEG, up to 4096x4096) and encoder `cedrus-jpeg-enc` | 0033-0036 + [`userspace/gstreamer/`](userspace/gstreamer/) |
| Cubietruck Bluetooth (AP6210) | setup fails on about half the boots; cannot scan or pair | works (crystal-gated power-up) | 0006-0008 + [`boards/cubietruck/bluetooth/`](boards/cubietruck/bluetooth/) |
| RTC accuracy (A20) | internal RC oscillator, -3.2 % | 32 kHz crystal, -28 ppm | 0006-0008 |
| HDMI audio (A10) | stream open times out; DMA channel leak per modeset | works | 0040, 0041 |
| SoC temperature | every read busy-waits 100 ms (10 % CPU on the A10); no hwmon device; oops after a GPADC module reload | fixed; `sensors`/htop show the SoC temperature (`cpu_thermal`) | 0039, 0047-0049 |
| MK802 board support | no PMIC (no DVFS), WiFi of the RTL8188EUS revision dead | AXP209, WiFi on PH3 | 0037 |

Wayland: zero-copy video works on a patched Weston, not on wlroots compositors; the desktop stays
on Xorg for now ([`userspace/wayland/`](userspace/wayland/)).

## Layout

| Path | What |
|---|---|
| [`kernel/sunxi-6.18/`](kernel/sunxi-6.18/) | the series 0001-0049 for Linux 6.18.56 as Armbian userpatches, with an index |
| [`kernel/upstream/`](kernel/upstream/) | series prepared for the mainline kernel (not sent yet) |
| [`armbian/`](armbian/) | Armbian build configs (A20, A10), the Cubietruck DRAM 432 MHz u-boot patch, build and install steps |
| [`overlays/`](overlays/) | device-tree overlays (CMA pools, Cubietruck Bluetooth, A20 1008 MHz) |
| [`boards/cubietruck/`](boards/cubietruck/) | Cubietruck guide (measurements, limits, verification, Bluetooth, DRAM, tips, troubleshooting) and the Bluetooth service |
| [`boards/mk802/`](boards/mk802/) | MK802 (A10) notes |
| [`userspace/vdpau-sun4i/`](userspace/vdpau-sun4i/) | VDPAU driver `libvdpau_sun4i-drm` for zero-copy H.264 playback, with test scripts |
| [`userspace/xserver-g2d/`](userspace/xserver-g2d/) | patch for Debian Trixie's X server: glamor copies on the 2D engine |
| [`userspace/sun4iximagesink/`](userspace/sun4iximagesink/) | GStreamer video sink: zero-copy playback on an overlay plane under X |
| [`userspace/gstreamer/`](userspace/gstreamer/) | two small GStreamer format-mapping patches |
| [`userspace/wayland/`](userspace/wayland/) | experimental Weston and waylandsink patches |
| [`tools/check-public.sh`](tools/check-public.sh) | consistency check of the patch headers |
| [`LICENSES.md`](LICENSES.md) | licences per directory |

## Quick start

1. Build a kernel with the series: [`armbian/README.md`](armbian/README.md).
2. Install it and the overlays, then follow the board guide:
   [Cubietruck](boards/cubietruck/README.md), [MK802](boards/mk802/README.md).
3. For the desktop: the patched X server ([`userspace/xserver-g2d/`](userspace/xserver-g2d/)) and
   the VDPAU driver ([`userspace/vdpau-sun4i/`](userspace/vdpau-sun4i/)).

## Upstream status

Nothing has been sent yet. Series for the mainline kernel are prepared for the lima fixes, the
v4l2-jpeg parser fix, the sun4i frontend/backend work (second pipeline, cursor, async flips,
scaler), VGA (as an RFC) and the RTC crystal; see [`kernel/upstream/`](kernel/upstream/). The 2D
engine driver needs an RFC first (new uAPI). The video engine work builds on Bootlin's unmerged
cedrus rework, so upstreaming it needs coordination with its authors. 0001-0003 and 0037 are local
only; 0040/0041 fix patches that only Armbian carries.

## Credits

Written by D.R.racer `<drracer@seznam.cz>` with the help of an AI assistant (Claude); every change
was reviewed and tested on the hardware, and every patch says so with an `Assisted-by:` trailer.
The VGA device tree builds on Chen-Yu Tsai's 2019 device tree work for the same board. The frontend
filter table (0016) and several register settings come from Allwinner's GPL-licensed 3.4 kernel as
published by the linux-sunxi project. The video engine driver builds on Paul Kocialkowski's
(Bootlin) cedrus rework and H.264 encoder (0023/0024) and on the cedrus driver by Bootlin and
others; the A20 encoder and JPEG work used Jens Kuske's research on the A20 video engine. The VDPAU
driver follows the structure of Jens Kuske's libvdpau-sunxi for the 3.4 kernel. The lima fixes
follow what ARM's Mali-400 r3p0 kernel driver does.

Licences: [`LICENSES.md`](LICENSES.md) (kernel patches GPL-2.0, X server patch MIT/X11, VDPAU
driver MIT, documentation CC-BY-4.0).
