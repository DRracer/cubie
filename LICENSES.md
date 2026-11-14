# Licences

The full licence texts are in [`LICENSES/`](LICENSES/): GPL-2.0, LGPL-2.1, MIT (template;
the copyright line is in each file or below) and CC-BY-4.0.

- **Kernel patches** (`kernel/`): GPL-2.0, the licence of the Linux kernel files they change or
  add, as given by the `SPDX-License-Identifier` of each file. New files with other identifiers, as
  usual for their type: the 2D engine uAPI header `include/uapi/drm/sun4i_mp_drm.h` (0021) is MIT,
  the encoder uAPI header `uapi/cedrus-enc.h` (0032) is (GPL-2.0+ WITH Linux-syscall-note) OR MIT,
  the decoder uAPI header `uapi/cedrus-dec.h` (0035) GPL-2.0 WITH Linux-syscall-note, the binding of
  0019 GPL-2.0-only OR BSD-2-Clause.
  Patch 0016 contains a coefficient table from Allwinner's A10/A20 3.4 kernel
  (`drivers/video/sunxi/disp/de_be.c`, Copyright (C) 2007-2012 Allwinner Technology Co., Ltd.,
  GPL-2.0-or-later). Patches 0023 and 0024 contain work by Paul Kocialkowski / Bootlin (cedrus
  rework and H.264 encoder, GPL-2.0 like the cedrus driver), credited in the patch descriptions
  and in the kept copyright headers.
- **X server patch** (`userspace/xserver-g2d/`): X11/MIT licence, like the X server it changes
  (the notice is in the new file `glamor/glamor_g2d.c`).
- **GStreamer patches** (`userspace/gstreamer/`, `userspace/wayland/gst-wayland/`):
  LGPL-2.1-or-later, the licence of the GStreamer files they change.
- **GStreamer video sink** (`userspace/sun4iximagesink/`): LGPL-2.1-or-later, like GStreamer
  plugins in general (Copyright (C) 2026 D.R.racer; text in its `COPYING`).
- **Weston patches** (`userspace/wayland/weston/`): MIT, the licence of Weston.
- **Bluetooth scripts** (`boards/cubietruck/bluetooth/`): MIT (Expat) licence, the same text as
  the VDPAU driver below (Copyright (c) 2026 D.R.racer).
- **VDPAU driver** (`userspace/vdpau-sun4i/`): MIT (Expat) licence, as most VDPAU and Mesa drivers:

  > Copyright (c) 2026 D.R.racer
  >
  > Permission is hereby granted, free of charge, to any person obtaining a copy of this
  > software and associated documentation files (the "Software"), to deal in the Software
  > without restriction, including without limitation the rights to use, copy, modify, merge,
  > publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
  > to whom the Software is furnished to do so, subject to the following conditions:
  >
  > The above copyright notice and this permission notice shall be included in all copies or
  > substantial portions of the Software.
  >
  > THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
  > INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
  > PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
  > FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
  > OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
  > DEALINGS IN THE SOFTWARE.

  It is an independent implementation for the mainline interfaces (V4L2 requests, DRM planes).
  Its file layout and some type names follow libvdpau-sunxi by Jens Kuske (LGPL-2.1-or-later),
  the driver for the vendor 3.4 kernel; the lines both have in common are VDPAU API names and
  boilerplate. The same text is in `userspace/vdpau-sun4i/LICENSE`.
- **Device-tree overlays** (`overlays/*.dts`): GPL-2.0-or-later OR MIT, like most Allwinner
  device trees in the kernel.
- **U-Boot patch** (`armbian/userpatches/u-boot/`): GPL-2.0-or-later, the licence of U-Boot.
- **Armbian configs** (`armbian/userpatches/*.conf`): GPL-2.0, the licence of the Armbian build
  framework.
- **Documentation** (`README.md` files): CC-BY-4.0.
- **Scripts** (`tools/`): MIT (Expat), same text as above.

Author: D.R.racer <drracer@seznam.cz>.
