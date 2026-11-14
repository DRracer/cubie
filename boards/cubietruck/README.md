# Cubietruck (Allwinner A20, 2 GiB)

User guide for running a Cubietruck (Cubieboard 3) as a desktop on mainline Linux 6.18 with the
patches of this repository: GPU, VGA, dual head, hardware cursor, 2D engine, zero-copy hardware
video playback, hardware H.264 and JPEG encoding/decoding, Bluetooth.

Everything here was tested on one Cubietruck (A20, 2 GiB) running Armbian Trixie (community build
26.11 trunk) with kernel 6.18.56, Mesa 25.0, Xorg 21.1.16 (modesetting + glamor), LXQt. The tiled
video crop fix (0004), the overlay plane patches (0001, 0002) and the video engine patches were
first tested on a Cubieboard2 (A20, 1 GiB). The current test kernel has all of 0001-0046. Other
A20 boards are untested; the kernel patches are written for the A10/A20 in general, the overlays
and the Bluetooth notes are Cubietruck specific.

- Building and installing the kernel: [`../../armbian/README.md`](../../armbian/README.md)
- Kernel patch index ("needed for", upstream state):
  [`../../kernel/sunxi-6.18/README.md`](../../kernel/sunxi-6.18/README.md)
- Patched X server (2D engine): [`../../userspace/xserver-g2d/`](../../userspace/xserver-g2d/)
- VDPAU driver (video playback): [`../../userspace/vdpau-sun4i/`](../../userspace/vdpau-sun4i/)
- Bluetooth service and scripts: [`bluetooth/`](bluetooth/)
- Device-tree overlays: [`../../overlays/`](../../overlays/)

## Measured numbers

Most numbers were measured with an earlier state of the series (0001-0029).

- **GPU:** before the fix nothing rendered at all. With it, glmark2 on a single
  1280x1024 head: 72 windowed / 104 off-screen at DRAM 384 MHz, 78 / 108 at 432 MHz. The vendor
  3.4 kernel with the Mali blob scored 108-119 on the same kind of board. Off-screen, scene by
  scene, lima is on par with the blob (geometric mean 1.05: slower in shader-heavy scenes, faster in
  blur/shadow/buffer scenes); the windowed gap was the X server
  copying each frame on the GPU (the 3.4 driver scanned GL windows out directly), which the
  2D engine now takes over (below).
- **Dual head** (HDMI 1080p + VGA 1280x1024): glmark2 windowed 54 (DRAM 384) / 62 (DRAM 432),
  stock X server.
- **2D engine in X** (dual head, DRAM 432, CPU 1008): glmark2 windowed `build` 73 -> 108 FPS,
  `texture` 77 -> 111 FPS; x11perf `scroll500` 104 -> 391/s, `copywinwin500` 107 -> 425/s
  (details and caveats in `../../userspace/xserver-g2d/README.md`). The engine alone: copies
  106-117 Mpixels/s with both heads on (130-140 with one head).
- **Async page flips:** glmark2 `--fullscreen` (no vsync) on one 1080p HDMI head, build scene:
  29 FPS without (X copies every frame), 108 FPS with the patch (note: 29 was measured at DRAM
  384 MHz, 108 at 432 MHz; the DRAM clock alone accounts for 4-14 %).
- **DRAM 432 MHz vs 384 MHz:** glmark2 +4 % (single head, off-screen) to +14 % (dual head,
  windowed), geometric means over the scenes. memtester (800 MB) ran a whole night next to GPU
  stress without a failure; displays clean.
- **CPU 1008 MHz vs 960 MHz:** CPU-bound work ~5 % faster; glmark2 and x11perf unchanged.
- **Video:** 1080p25 H.264 (VDPAU, windowed) at ~27 % CPU. Synthetic 1080p50: ~1 % dropped frames
  on HDMI, on VGA, and with the window split across both monitors. Mouse movement during video:
  with the software cursor a 1080p25 clip had 155-196 late frames per 20 s while the mouse moved
  (56 with the mouse still); with the hardware cursor a 1080p50 clip drops 22 of 999 frames while
  the mouse moves (12 with the mouse still). The clips differ, so read this as "the mouse no
  longer matters much", not as an exact factor.
- **Video engine:** with patches 0023-0029, Big Buck Bunny 1080p (300 frames) and a 1080p25 TV
  clip (1547 frames) decode bit-exact against FFmpeg's software decoder; the engine runs at
  192 MHz while decoding (96 MHz without 0028: 1080p H.264 at 22 instead of 41 fps). The encoder
  passed its regression suite (I- and P-frame sessions, rate control) on this kernel.
- **JPEG (0033-0036):** decoding a 1024x768 4:2:2 webcam frame takes about 16 ms of video engine
  time; encoding runs at 35 fps at 1080p and 88 fps at 1024x768 (quality 75).
- **SATA:** 152-179 MB/s sequential read (direct I/O) on clean regions of an SSD = host limit.

## Known limits

- **DRAM bandwidth is the bottleneck.** Scanout of two monitors (~810 MB/s for 1080p + 1280x1024
  at 32 bpp) slows the GPU noticeably; GL-heavy use is faster on one head. DRAM 432 MHz helps.
- **Video downscaling beyond what the frontend can fetch is refused** (the plane is rejected, the
  video window stays empty). For a 1080p video on a 1080p60 head that is beyond about 4x
  vertically (480x270 plays, 384x216 is refused). Beyond about 2x the frontend skips every other
  source line, which aliases a bit more. The limits were measured at DRAM 432 MHz on one head; at
  384 MHz or with two heads the margin is smaller.
- **glmark2 `jellyfish` shows a growing checkerboard** on the jellyfish after a long run. It
  happens with the Mali blob on 3.4 too: Mali-400 fragment shaders are fp16 only, so the
  shader's time-based texture coordinate loses precision. Harmless in itself.
- **Rare lima GPU faults are fixed by 0042-0046.** Before these patches, a `gpmmu page fault`
  could hang the GPU after hours of GPU load (seen once on the Cubietruck, `at 0x9000`, after
  2 h 50 min of glmark2). The cause was found on an A10 board, where it was easy to reproduce:
  when lima grows a GPU heap, it flushed the GPU's L2 cache and TLB before mapping the new pages
  instead of after (0046), and the recovery from such a fault was broken in several ways
  (0042-0045). The Cubietruck fault is very likely the same bug. A different lima failure
  (`pmu timeout`, runtime resume) was seen once at DRAM 384 MHz; its cause is unknown. If your
  desktop stops drawing, see "Troubleshooting".
- **HDMI picture dropouts under heavy load** (the picture goes blank for about a second and comes
  back; no hot-plug event). Seen on HDMI only (VGA stayed stable), also with the 3.4 kernel and on
  another A20 board (Cubieboard2). Cause unknown, not pursued.
- **The 32 kHz crystal stops when the board runs warm** (SoC about 43.5 °C under sustained load on
  the test board); see "Bluetooth and the RTC crystal". Cool the board (a fan helps).
- **Async flips are refused while a video plane is on screen** (X falls back to copying).
- **Video playback:** the video plane is always on top (no colour key yet: menus over the video
  are hidden), and mpv's on-screen controller costs CPU while visible; see
  `../../userspace/vdpau-sun4i/README.md`.
- **Hardware H.264 decoding:** on synthetic test clips, a few chroma values at the bottom edge
  can differ by 1-2 from software decoding (seen on cedrus with any player); real content was
  bit-exact.
- The Cubietruck kernel config selects the Cortex-A7 code generation target of patch 0003
  (`ARMV7_TUNE_CORTEX_A7`): that kernel **runs only on the A20**. For A10/A13 boards select the
  Cortex-A8 target or keep the generic default.

## Pitfall: out-of-tree modules and Armbian's modules.dep

Armbian's kernel package ships a prebuilt `modules.dep`, which knows nothing about modules you
added yourself. Rebuilt kernels keep the same version string (`6.18.56-current-sunxi`), so a
module under `/lib/modules/<version>/updates/` (for example an earlier out-of-tree build of the
cedrus driver) stays in place across kernel installs, but after each install modprobe silently
loads the **in-tree** module of the same name again, until `sudo depmod -a` is run. On the test
board this brought back slow, wrong video decoding after a kernel update. That is why the video
engine driver is in the kernel patches (0023-0036) and not an out-of-tree module.
If you used an out-of-tree `sunxi-cedrus.ko` before, remove it from `updates/` and run
`sudo depmod -a` (with the new kernel running; without a version argument depmod works on the
running kernel). In general, run `depmod -a` after every kernel install if you keep modules in
`updates/`.

## Verify your install

```sh
uname -v; dpkg -l 'linux-image-*sunxi*' | tail -1     # your build
dmesg | grep 'mixer processor'          # "mixer processor at 216 MHz"  (2D engine, 0018-0022)
dmesg | grep 'VE version'               # "VE version 0x1623, module clock ... MHz"  (0023-0029)
v4l2-ctl --list-devices                 # cedrus: decoder, encoder, cedrus-jpeg, cedrus-jpeg-enc
grep 'G2D:' /var/log/Xorg.0.log         # "G2D: enabled on /dev/dri/renderD129 ..."  (xserver-g2d)
cat /sys/module/sun4i_mp/parameters/allow_overlap    # Y if you set it
sudo grep pll-ddr /sys/kernel/debug/clk/clk_summary  # 432000000 with the u-boot option
```

The `VE version` line comes from this repository's video engine driver (the stock driver does not
print it); the clock it shows depends on the moment it was printed, because the GPU driver
retunes the PLL the two share. What matters is the clock while decoding: play a video and run
`sudo grep -w ve /sys/kernel/debug/clk/clk_summary`, which must show 192000000. If there is no
`VE version` line at all (it can also appear only after the first decode), an old cedrus module
may be loaded (see the pitfall above): `modinfo -n sunxi-cedrus` must point into
`kernel/drivers/staging/media/sunxi/cedrus/`, not `updates/`. For a full decoder check, run
`../../userspace/vdpau-sun4i/vdpau-test.sh` on a few H.264 clips.

## Hardware H.264 encoding

The video engine driver registers a second video device for the encoder (`v4l2-ctl
--list-devices` lists both under `cedrus`). Stock FFmpeg uses it through `h264_v4l2m2m`, e.g.
from a camera or a raw NV12 file:

```sh
ffmpeg -f rawvideo -pix_fmt nv12 -video_size 640x480 -framerate 15 -i in.nv12 \
       -c:v h264_v4l2m2m -b:v 1M -g 30 -num_output_buffers 4 out.264
```

`-num_output_buffers 4` matters at large sizes: FFmpeg's default of 16 input buffers needs about
50 MB of the shared contiguous memory at 1080p (about 12 MB with 4). Rate control: `-b:v`
(constant bit rate) or a fixed QP with `-qmin N -qmax N` (both in the encoder regression suite).
Pictures must be at least 64 pixels wide; the largest size is 1920x1088. Hardware decode ->
hardware encode (transcoding) works, but the decoder's tiled output has to be converted by the
CPU in between, which limits it to a few frames per second at 1080p; real-time 1080p transcoding
is not possible this way.

For motion detection, patch 0032 exports the encoder's per-macroblock motion vectors of each
P-frame through the read-only control `V4L2_CID_CEDRUS_ENC_MB_INFO` (enabled with
`V4L2_CID_CEDRUS_ENC_MB_INFO_ENABLE`; layout in the driver's `uapi/cedrus-enc.h`,
`drivers/staging/media/sunxi/cedrus/uapi/` in the patched kernel).

## Hardware JPEG

Patches 0033-0036 add two more video devices; clients should find them by card name, not by
`/dev/videoN` number:

- **`cedrus-jpeg`**, a stateful decoder: JPEG or MJPEG in, one frame per buffer; out
  macroblock-tiled pictures, `NV12_32L32` for 4:2:0 and a driver-private tiled NV16 (`ST16`) for
  4:2:2 (4:4:4 input is averaged to 4:2:2 by the engine). Baseline JPEG, with or without Huffman
  tables (UVC webcams leave them out), restart intervals, 16x16 up to 4096x4096. This is what lets
  a UVC webcam's MJPEG stream be decoded without the CPU. Progressive, gray, 4:1:1 and 4:4:0
  pictures are refused. The display can scan the tiled pictures out directly; there is no linear
  output, so feeding them to the encoder needs one CPU de-tiling step (about 3 ms per 1024x768
  frame).
- **`cedrus-jpeg-enc`**, an encoder: linear NV12 in, baseline JFIF (4:2:0) out,
  `V4L2_CID_JPEG_COMPRESSION_QUALITY` 1-100 (default 75; the standard tables scaled as libjpeg
  does). The engine rounds even quantisers half a step early, so quality 99 gains nothing over
  98: use 97 or 100 for the top end.

GStreamer's `v4l2jpegdec` needs the patch in `../../userspace/gstreamer/` (it maps
`NV12_32L32`); with it 4:2:0 JPEG decodes, 4:2:2 (the private `ST16` format) does not.

Truncated frames from a camera: a scan that does not end with EOI is rejected (the engine would
wait for more data forever). A frame that was cut short but still ends with EOI can decode
"successfully" with the rest of the picture filled flat; the hardware gives no way to tell. A
4096x4096 picture needs a 32 MiB buffer, so only a few fit in the contiguous memory pool.

## Bluetooth and the RTC crystal

Three pieces make Bluetooth work:
1. `CONFIG_SERIAL_8250_16550A_VARIANTS=y` (in the Armbian config): the Bluetooth UART gets
   hardware flow control, and the controller setup (firmware patch, address) succeeded in 90 of 90
   driver re-binds. Without it, it fails on about half of the boots.
2. Patches 0006-0008: LOSC, the A20's 32 kHz oscillator, runs from the 32.768 kHz crystal instead
   of the internal RC oscillator. LOSC is also the AP6210's low-power clock, and the module samples
   it when it is powered up: powered up on the RC oscillator (or on a 32.000 kHz clock derived
   from the 24 MHz oscillator), it cannot scan or pair until it is powered up again with the
   crystal running.
3. [`bluetooth/`](bluetooth/): a service that powers the module up only once the crystal runs.
   This is needed because on the test board the crystal **stops when the A20 runs warm** (SoC
   sensor about 43.5 °C under sustained CPU/GPU load), and the RTC then falls back to the RC
   oscillator by itself until the board cools down. A module that was powered up with the crystal
   running keeps working through that; only a power-up on a hot board (a boot under load) is
   affected, and the service then repeats it once the board has cooled. Details and install
   steps: `bluetooth/README.md`.

With a fan the crystal kept running under the heaviest load we tried. Reports from other
Cubietrucks (does the crystal stop when warm, and at what temperature?) are welcome.

## Overlays

Copy the `.dts` files from `../../overlays/` to the board and add each with Armbian's tool, which
compiles it into `/boot/overlay-user/` and adds it to `user_overlays=` in `/boot/armbianEnv.txt`
(it needs the device tree compiler, package `device-tree-compiler`):

```sh
sudo apt install device-tree-compiler
sudo armbian-add-overlay sun7i-a20-cma-224m.dts
sudo armbian-add-overlay sun7i-a20-cubietruck-bt.dts     # edit the address in it first
sudo armbian-add-overlay sun7i-a20-cpu-1008.dts          # optional, see below
```

`/boot/armbianEnv.txt` should then contain, besides your other settings:

```
user_overlays=sun7i-a20-cma-224m sun7i-a20-cubietruck-bt
bootm_size=0x20000000
```

- **`sun7i-a20-cma-224m`** enlarges the contiguous memory pool from 96 to 224 MiB, which the video
  decoder, the display and the GPU buffers share. The pool must stay inside the first 256 MiB of
  DRAM, the only part the video engine can reach; the overlay keeps it there. Do not use the
  `cma=` kernel argument instead: it moves the pool out of that window. **`bootm_size=0x20000000`
  in `armbianEnv.txt` is required:** without it u-boot places the initrd and the device tree
  inside that window, the enlarged pool no longer fits, and the kernel silently puts it elsewhere
  (the video engine driver then refuses to load).
- **`sun7i-a20-cubietruck-bt`** (works without patches 0006-0008, although the comment in the
  file names them; only the radio question above depends on them) keeps the Bluetooth UART at
  115200 baud (at the default 1.5 Mbaud the controller stops answering after the speed change)
  and gives the controller a Bluetooth address. **Edit `local-bd-address` before installing**:
  the file ships with all zeros, which the kernel ignores (Bluetooth then stays unconfigured);
  every board needs its own address (bytes in reverse order, see the comment in the file).
  Bluetooth also needs the AP6210 firmware from Armbian's firmware package (`armbian-firmware`,
  `/lib/firmware/ap6210/`) under the name the kernel looks for:

  ```sh
  sudo ln -sfn ../ap6210/bcm20710a1.hcd "/lib/firmware/brcm/BCM20702A1.cubietech,cubietruck.hcd"
  ```

- **`sun7i-a20-cpu-1008`** (optional) adds a 1008 MHz @ 1.45 V operating point, the highest entry
  of the Cubietruck's vendor (3.4) configuration (above mainline's tested operating points). On mainline: CPU-bound work ~5 %
  faster, GPU-bound work unchanged; the test board without a fan peaked at 57 degC during an
  overnight GPU + memory stress test. Watch `/sys/class/thermal/thermal_zone0/temp` if your board
  has no fan.

## Optional: DRAM at 432 MHz (u-boot)

Armbian lowers the Cubietruck's DRAM clock from mainline u-boot's 432 MHz to 384 MHz "for
stability". 432 MHz gives 12.5 % more memory bandwidth, which the GPU and the display share; on
the test board it ran memtester and GPU stress overnight without errors. Your board may differ:
**keep a way to restore the old bootloader** (another PC with an SD card reader).

Test after flashing, e.g. `sudo apt install memtester; sudo memtester 800M 1` (takes long; 0
failures expected) while a GL program runs, and watch both monitors for artefacts.

1. In the Armbian build tree, put `0001-cubietruck-dram384-emmc-slot.patch` from
   `../../armbian/userpatches/u-boot/v2026.07-sunxi/board_cubietruck/` into
   `userpatches/u-boot/v2026.07-sunxi/board_cubietruck/` (with `USERPATCHES_PATH` pointing to
   `../../armbian/userpatches` it is already there). The file name must be the same as Armbian's
   own patch so that it replaces it (it keeps Armbian's eMMC slot change and drops the clock
   change).
2. `./compile.sh uboot BOARD=cubietruck BRANCH=current`
3. Unpack `output/debs/linux-u-boot-cubietruck-current_*.deb` (`dpkg-deb -x <deb> x`); the
   bootloader is `x/usr/lib/linux-u-boot-current-cubietruck/u-boot-sunxi-with-spl.bin`. Check
   `x/usr/lib/u-boot/Cubietruck_config` for `CONFIG_DRAM_CLK=432`.
4. On the Cubietruck (SD card = `/dev/mmcblk0`; check with `lsblk`), back up the bootloader area
   first, then write the new one. It starts at 8 KiB and must not touch the partition table:

   ```sh
   sudo dd if=/dev/mmcblk0 of=uboot-backup-8k-1m.bin bs=1k skip=8 count=1016
   sudo dd if=u-boot-sunxi-with-spl.bin of=/dev/mmcblk0 bs=1k seek=8 conv=notrunc,fsync
   sudo reboot
   ```

   Restore (from the board, or from a PC with the card in a reader as `/dev/sdX`):
   `sudo dd if=uboot-backup-8k-1m.bin of=/dev/sdX bs=1k seek=8 conv=notrunc,fsync`.
5. After the reboot, `sudo grep pll-ddr /sys/kernel/debug/clk/clk_summary` shows 432000000.

Armbian's own bootloader updates (`armbian-config`, package `linux-u-boot-cubietruck-current`)
write the 384 MHz version back; hold that package if you keep 432 MHz.

## Desktop tips

- **CPU governor:** frequency changes stall the CPU briefly and caused audio gaps during video
  playback. Keep the CPU at its top frequency (as the vendor 3.4 setups did): on Armbian set
  `GOVERNOR=performance` (and `MIN_SPEED` to the top frequency) in `/etc/default/cpufrequtils`,
  because Armbian's boot-time hardware optimisation applies that file and overrides
  `cpufreq.default_governor=performance` on the kernel command line. A busy board runs warmer;
  see "Bluetooth and the RTC crystal".
- **Background jobs:** the two cores have little headroom; anything that starts a new process
  every few seconds (e.g. a shell loop calling python3) caused audible audio dropouts.
- **Analog audio** (3.5 mm jack): set the codec's power amplifier to 0 dB once
  (`amixer -c0 sset "Power Amplifier" 63; sudo alsactl store 0`); PipeWire only uses software
  volume, and the default -31 dB is nearly inaudible. A larger PipeWire ALSA headroom
  (`api.alsa.headroom = 2048` for the codec sink in a WirePlumber rule) removed the remaining
  underruns.
- **Compositor off:** the xfwm4 compositor halves GL performance (glmark2 67 -> 36).
- **Dual head:** e.g. `xrandr --output HDMI-1 --mode 1920x1080 --left-of VGA-1`.
- **VGA monitor without EDID:** the driver offers standard modes up to 1920x1200, preferring
  1024x768.
- **IR receiver:** `ir-keytable -s rc0 -p nec` (enable only the protocols you need; `all` produces
  false decodes), then a keymap in `/etc/rc_keymaps/`.
- **Input voltage:** the AXP209 measures the DC jack input:
  `/sys/bus/iio/devices/iio:deviceN/in_voltage0_raw` x 1.7 mV and `in_current0_raw` x 0.625 mA
  (pick the device whose `name` is the AXP's ADC; the numbering can change between boots). The
  board is rated 5.2 V / 2 A; readings below ~4.8 V under load (SATA drive, USB devices) mean a
  weak supply or cable.
- **SATA power cycle without a reboot** (a hung drive): unbind and bind `ahci-sunxi`
  (`echo 1c18000.sata | sudo tee /sys/bus/platform/drivers/ahci-sunxi/unbind`, wait, same with
  `bind`); the driver switches the SATA 5 V supply off and on.

## Troubleshooting

- **Black screen, `lima ... gpmmu page fault` in dmesg right from the start of X:** the Mali
  patch (0005) is missing; check `uname -v` / the package version. (A single fault after hours
  of GPU load points to a kernel without 0042-0046; see "Known limits".)
- **Desktop stops drawing (panel or icons invisible), `lima ... pmu timeout` or GPU hang:**
  restart the GPU without a reboot:
  `sudo systemctl stop sddm; echo 1c40000.gpu | sudo tee /sys/bus/platform/drivers/lima/unbind;
  echo 1c40000.gpu | sudo tee /sys/bus/platform/drivers/lima/bind; sudo systemctl start sddm`
  (patch 0009 makes this safe on 2 GiB boards).
- **Bluetooth: `hci0` has no address or `Reading local name failed (-110)`:** check
  `CONFIG_SERIAL_8250_16550A_VARIANTS=y` (`zgrep VARIANTS /proc/config.gz` or the config in
  `/boot`), the firmware link and the overlay. Re-binding the driver restarts the setup:
  `echo serial0-0 | sudo tee /sys/bus/serial/drivers/hci_uart_bcm/unbind` and the same with
  `bind`.
- **Bluetooth: setup fine, but scans find nothing / pairing fails:** the module was powered up
  while the crystal was not running (board too hot, or patches 0006-0008 missing).
  `journalctl -u cubietruck-bt -p warning` tells; cool the board, then `sudo cubietruck-bt rebind`
  (the service also does it by itself).
- **Video slow (1080p H.264 well below real time) or with displaced blocks in moving scenes:**
  the stock cedrus module is loaded instead of the patched one; see "Verify your install" and
  "Pitfall: out-of-tree modules".
- **Video window stays empty (desktop colour shows):** the video is downscaled more than the
  display frontend can fetch (see "Known limits"); make the window bigger. `dmesg` is quiet about
  it; `VDPAU_SUN4I_DEBUG=1 mpv ...` prints `video plane: Invalid argument` for each refused frame.
- **No `G2D: enabled` in the Xorg log:** the log says why (`no accessible sun4i-mp render node`:
  driver not loaded or no access to `/dev/dri/renderD*`; `disabled by ...`: switched off by
  option or environment). Copies then simply run on the GPU as before.
- **Monitor switched on after boot stays dark at the SDDM login:** run `xrandr --output HDMI-1
  --auto` in the X session (or an SDDM `DisplaySetupScript` that does it).
