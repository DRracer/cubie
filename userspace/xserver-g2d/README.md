# X server: glamor copies on the A20 2D engine (G2D)

`99_glamor-g2d-sun4i-mp.diff` is a patch for Debian Trixie's X server (source package
`xorg-server`, version `2:21.1.16-1.3+deb13u4`). With it, glamor hands copies between
physically contiguous buffers to the A20's 2D engine (the "mixer processor", G2D in the vendor
kernel) through the `sun4i-mp` kernel driver (kernel patches 0018-0022) instead of running them
as Mali GPU jobs. That covers the front buffer (scrolling, moving windows, `CopyArea`) and the
Present copies of GL windows (each frame of a windowed GL program). Everything else stays on the
unchanged glamor GL path, and so does any copy the engine refuses or fails.

Why it helps: on the A20, the X server copies every frame of a windowed GL program into the front
buffer, and on the Mali-400 such a copy costs a full tile reload and write-back. The 2D engine
copies at about 110-140 Mpixels/s (bound by DRAM bandwidth), in parallel with the GPU, and
should need less DRAM traffic per copy (estimated 8 instead of about 12 bytes per pixel; not
measured).

## Results (Cubietruck, HDMI 1080p + VGA 1280x1024, DRAM 432 MHz, CPU 1008 MHz)

| Test | glamor (stock) | with the patch |
|---|---|---|
| glmark2 windowed 800x600, `build` | 73 FPS | 108 FPS |
| glmark2 windowed 800x600, `texture` | 77 FPS | 111 FPS |
| x11perf `-scroll500` (needs `allow_overlap=1`) | 104/s | 391/s |
| x11perf `-copywinwin500` | 107/s | 425/s |
| x11perf `-putimage500` / `-shmput500` | 63/s / 271/s | 85/s / 341/s |

With the patch, the X server submits no GPU jobs for these copies any more (checked with a GPU
trace); the Mali fragment processor is 37 % busy in `build` instead of 97 %. The gain is
largest where the copy dominates (`build`, `texture`); a full glmark2 suite gains less. Scrolling
in terminals and browsers is visibly faster.

## Requirements

- A kernel with patches 0018-0022 and `CONFIG_DRM_SUN4I_MP=m` (the Armbian config in
  `../../armbian/` sets it). `dmesg | grep 'mixer processor'` must show
  `mixer processor at 216 MHz`, and a render node such as `/dev/dri/renderD129` belongs to it.
- Xorg with the `modesetting` driver and glamor (Armbian's default on the A20 with lima).
- The render node must be readable and writable by the X server process. That is no issue for an
  X server running as root; a rootless X server needs the node to be world-accessible
  (`crw-rw-rw-`, Debian's default for render nodes) or the user in group `render`.

## Building the packages

The patch is a Debian quilt patch; add it to the Debian source package and rebuild. The build
needs a Debian Trixie system for armhf: either natively on an A20 board (slow, several hours;
needs swap) or cross-compiled on a PC, e.g. in a `debian:trixie` container. Cross build:

```sh
# Debian Trixie, with deb-src entries enabled in /etc/apt/sources.list.d/debian.sources
# (Types: deb deb-src) and the trixie-security suite listed
sudo dpkg --add-architecture armhf
sudo apt update
sudo apt install build-essential crossbuild-essential-armhf devscripts quilt fakeroot
sudo apt build-dep -a armhf -Pnocheck xorg-server
apt source xorg-server=2:21.1.16-1.3+deb13u4      # unpacks xorg-server-21.1.16/
cd xorg-server-21.1.16
cp /path/to/cubie/userspace/xserver-g2d/99_glamor-g2d-sun4i-mp.diff debian/patches/
echo 99_glamor-g2d-sun4i-mp.diff >> debian/patches/series
QUILT_PATCHES=debian/patches quilt push -a        # all patches must apply
dch -v 2:21.1.16-1.3+deb13u4+g2d1 "glamor: offload copies to the A20 G2D (sun4i-mp)"
DEB_BUILD_OPTIONS="nocheck parallel=$(nproc)" dpkg-buildpackage -a armhf -b -uc -us -Pnocheck
```

The result is in the parent directory. Only two packages are needed on the board:
`xserver-xorg-core_21.1.16-1.3+deb13u4+g2d1_armhf.deb` (the changed one) and
`xserver-common_21.1.16-1.3+deb13u4+g2d1_all.deb` (unchanged, but `xserver-xorg-core` requires
the same version). Notes:

- The G2D code is in the glamor EGL module of Xorg (`libglamoregl.so`); Xwayland, Xephyr and
  Xvfb do not use it.
- No kernel headers are needed: the ioctl structures are copied into `glamor/glamor_g2d.c`.
- If `fakeroot` hangs in your container, run `dpkg-buildpackage` as root inside the container
  and hand the files back to your user afterwards.
- If Debian has released a newer `xorg-server` for Trixie, build that version instead (the patch
  should apply; check that `quilt push -a` is clean) and adjust the version numbers above. Never
  install a G2D package that is older than the security update you would otherwise have.

## Installing

On the board, keep the stock packages for rollback, install, and hold them so that an update does
not silently replace the patched server:

```sh
mkdir -p ~/xserver-stock && cd ~/xserver-stock
apt download xserver-xorg-core=2:21.1.16-1.3+deb13u4 xserver-common=2:21.1.16-1.3+deb13u4
cd /path/to/the/built/debs
sudo apt install ./xserver-common_21.1.16-1.3+deb13u4+g2d1_all.deb \
                 ./xserver-xorg-core_21.1.16-1.3+deb13u4+g2d1_armhf.deb
sudo apt-mark hold xserver-xorg-core xserver-common
echo "options sun4i_mp allow_overlap=1" | sudo tee /etc/modprobe.d/sun4i-mp.conf   # see below
sudo reboot
```

Check `/var/log/Xorg.0.log` (or `~/.local/share/xorg/Xorg.0.log` for a rootless server):

```
(II) modeset(0): G2D: enabled on /dev/dri/renderD129 (allow_overlap Y, min 4096 px, busy source: wait)
```

When the X server exits it logs one statistics line (`G2D: N copies (of M calls) ...`).

**The hold matters:** while it is set, Debian security updates for the X server are not
installed. Watch for them (`apt list --upgradable`, Debian security announcements for
`xorg-server`), rebuild the new version with the patch, and install that.

## Configuration

| What | xorg.conf (`Device` section of the modesetting driver) | environment of the X server |
|---|---|---|
| switch off | `Option "G2DAccel" "off"` | `XSERVER_G2D=0` |
| smallest copy given to the engine (pixels, sum of the rectangles; default 4096) | `Option "G2DMinPixels" "4096"` | `XSERVER_G2D_MIN_PIXELS=4096` |
| copy from a client buffer the GPU is still drawing | `Option "G2DBusySource" "wait"` (default: wait for the GPU in the kernel) or `"gl"` (give that copy to the GPU instead) | `XSERVER_G2D_BUSY=gl` |

Restart X after a change. `Option "G2DAccel" is not used` warnings in the log can be ignored:
the option is read after the check that prints the warning, and still works.

**`allow_overlap`** (kernel module parameter of `sun4i_mp`, default off): overlapping copies
inside one buffer (scrolling, moving a window over itself) only go to the engine when it is on;
the X server re-reads it every 10 seconds. It was verified on the hardware in all directions
(the kernel refuses the one direction the engine cannot do, a move to the right on the same
lines, and X then uses the GPU). `/etc/modprobe.d/sun4i-mp.conf` with
`options sun4i_mp allow_overlap=1` makes it permanent; at runtime:
`echo 1 | sudo tee /sys/module/sun4i_mp/parameters/allow_overlap`.

## Caveats

- With `G2DBusySource "wait"` (the default), the X server waits in the kernel until a GL client
  has finished drawing the frame it presents. A hung GPU job then stalls the X server for the
  kernel's fence timeout per frame. `"gl"` avoids that at the cost of a GPU copy for busy frames.
- After repeated engine failures (3 I/O errors or 3 timeouts in a row, one "no device", or 16
  other failures in a row), the server switches the engine off for the rest of the session and logs `G2D: disabled ...`; the failed part of each
  copy is redone on the GPU, so the picture stays right.
- Only copies are offloaded (no fills, blending or scaling), only between 32 bpp buffers of the
  same depth that are physically contiguous (front buffer, GL client buffers). Off-screen pixmaps
  that glamor allocates as GL textures stay on the GPU.
- Tested on one Cubietruck with LXQt: desktop use, x11perf, glmark2, and a GPU trace. **Not
  tested:** RandR mode changes and switching a head off/on, VT switches, log out/in, long-term
  memory/file-descriptor leaks, `G2DBusySource "gl"`, the automatic switch-off after failures,
  a rootless X server, and other desktops/window managers.
- The patch also touches shared glamor code (pixmap release hooks in `glamor_egl.c`), so a bug
  there could affect any X client, not only GL programs.

## Rollback

```sh
sudo apt-mark unhold xserver-xorg-core xserver-common
sudo apt install --allow-downgrades ~/xserver-stock/xserver-common_*.deb ~/xserver-stock/xserver-xorg-core_*.deb
sudo systemctl restart sddm      # or your display manager, or reboot
```

Without the saved packages: `sudo apt install --allow-downgrades xserver-xorg-core=2:21.1.16-1.3+deb13u4
xserver-common=2:21.1.16-1.3+deb13u4` (while Debian still serves that version). To only switch
the engine off, use `Option "G2DAccel" "off"`, e.g. in `/etc/X11/xorg.conf.d/20-g2d.conf`:

```
Section "Device"
    Identifier "modesetting"
    Driver "modesetting"
    Option "G2DAccel" "off"
EndSection
```

## Licence

The patch is under the X11/MIT licence of the X server (see the notice in `glamor/glamor_g2d.c`
inside the patch).
