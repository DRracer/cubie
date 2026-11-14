# Kernel patches for Linux 6.18.56 (Armbian sunxi-6.18 userpatches)

One series for the A10 and the A20. The patches apply in order on top of Armbian's own
`sunxi-6.18` patches; [`../../armbian/README.md`](../../armbian/README.md) shows how to build a
kernel with them. "Needed for" says which feature breaks if you leave a patch out.

Tested kernels: all of 0001-0046 on a Cubietruck (A20, 2 GiB) and on a Rikomagic MK802 (A10,
1 GiB). Each patch description says what was tested on which board.

| # | Patch | Needed for | Upstream |
|---|---|---|---|
| 0001 | drm: let an authenticated non-master show a driver-designated overlay plane | VDPAU player path (`../../userspace/vdpau-sun4i/`): an authenticated X client may use the overlay planes. **Security:** any program that may talk to your X server can then put an image over the whole screen (lock screen included); leave 0001/0002 out on multi-user machines | local only |
| 0002 | drm/sun4i: offer overlay layers 1 and 3 to non-master clients (parameter `client_planes`, default 0xa; 0 turns it off) | same as 0001 | local only |
| 0003 | ARM: select the ARMv7 code generation target (Cortex-A7 / Cortex-A8) | **optional** speed-up, a Kconfig choice: `ARMV7_TUNE_CORTEX_A7` builds an A20-only kernel (never boot it on an A10/A13), `ARMV7_TUNE_CORTEX_A8` the A10 one; the default keeps the generic flags | local only |
| 0004 | drm/sun4i: frontend: honour the source offset of tiled semi-planar buffers | video window partly off-screen | [prepared](../upstream/drm-sun4i-fe-scaler/v1/) |
| 0005 | soc: sunxi: mbus: add the A10/A20 Mali GPU | **GPU on 2 GiB boards** (black screen, `gpmmu page fault` without it) | being prepared (waits for a hardware check) |
| 0006 | rtc: sunxi: run LOSC from the external 32k crystal when present | RTC accuracy, Bluetooth scanning/pairing on the Cubietruck | [prepared](../upstream/rtc-losc/v1/) |
| 0007 | ARM: dts: sun7i: add the external 32k crystal | same | same |
| 0008 | ARM: dts: sun7i: cubietruck: run the RTC from the 32k crystal | same | same |
| 0009 | soc: sunxi: mbus: restore the DMA offset when a driver binds again | GPU / video engine / 2D engine after a driver reload on 2 GiB boards (without it: memory corruption) | being prepared (waits for a hardware check) |
| 0010 | drm/sun4i: tcon: support the TV encoder as VGA output on A10/A20 | VGA | [RFC prepared](../upstream/drm-sun4i-vga-rfc/v1/) |
| 0011 | drm/sun4i: tv: add VGA output for A10/A20 | VGA | same |
| 0012 | ARM: dts: sun7i: add the TV encoders and the Cubietruck VGA output | VGA | same |
| 0013 | drm/sun4i: backend: route the frontend of a second pipeline | video on the second head (dual head) | [prepared](../upstream/drm-sun4i-fe-second-pipeline/v1/) |
| 0014 | drm/sun4i: backend: expose the hardware cursor as a cursor plane | hardware cursor | [prepared](../upstream/drm-sun4i-cursor/v1/) |
| 0015 | drm/sun4i: backend: support async page flips (needs 0014) | fast fullscreen GL without vsync | [prepared](../upstream/drm-sun4i-async-flip/v1/) |
| 0016 | drm/sun4i: frontend: select the scaler filter by scaling ratio | alias-free video downscaling | [prepared](../upstream/drm-sun4i-fe-scaler/v1/) |
| 0017 | drm/sun4i: frontend: skip input lines to stay within its throughput | video downscaled more than ~2x; refuses planes the frontend cannot fetch instead of corrupting the screen | same |
| 0018 | clk: sunxi-ng: a10: export pll-ddr-other and pin the MP clock parent | 2D engine (mixer processor) | later, with 0021 |
| 0019 | dt-bindings: gpu: add the Allwinner A10/A20 mixer processor | same | same |
| 0020 | soc: sunxi: mbus: add the A10/A20 mixer processor | same (without it the engine writes 1 GiB off target on 2 GiB boards; the driver refuses to probe) | same |
| 0021 | drm/sun4i: add a driver for the A10/A20 mixer processor (`sun4i-mp`, needs `CONFIG_DRM_SUN4I_MP=m`, set by the Armbian configs) | same; used by `../../userspace/xserver-g2d/` | later (new uAPI, RFC first) |
| 0022 | ARM: dts: sun7i: add the mixer processor | same, A20 | same |
| 0023 | media: cedrus: rework the driver into platform, proc, context and engine | **hardware video decoding** (with 0025-0030) and all encoders | contains Bootlin's work, see below |
| 0024 | media: cedrus: add H.264 encoding for the V3s and the A10/A20 | H.264 hardware **encoding** (V4L2 M2M, e.g. FFmpeg `h264_v4l2m2m`) | same |
| 0025 | media: cedrus: A10/A20: refuse buffers outside the VE's 256 MB window | safety: the video engine only reaches the first 256 MB of DRAM; also refuses to bind without the MBUS DMA offset | later |
| 0026 | media: cedrus: decoder: honour cache hints on CAPTURE, optional `dec_cached_capture` | faster CPU read-back of decoded pictures (e.g. GStreamer de-tiling); off by default | later |
| 0027 | media: cedrus: print the VE version once at first power-up | the install check in the board guides | later |
| 0028 | media: cedrus: request the module clock rate at every power-up | **decoding speed**: without it the VE runs at 96 instead of 192 MHz after boot (1080p H.264 22 instead of 41 fps) | later |
| 0029 | media: cedrus: keep a kernel mapping of VE buffers on ARM32 | **correct H.264 decoding**: without it B-frames decode with displaced blocks (cache-alias bug) | later; the proper fix belongs in arch/arm or dma-direct |
| 0030 | media: cedrus: free the controls after releasing the M2M context | use-after-free when a file is closed with requests in flight | later |
| 0031 | media: cedrus: h264 encoder: number the buffers of both queues | `v4l2_buffer.sequence` on the encoder (v4l2-compliance, lost-frame detection) | later |
| 0032 | media: cedrus: h264 encoder: export per-MB information as a control | motion vectors / MB modes of P-frames for motion detection (`uapi/cedrus-enc.h`) | later |
| 0033 | media: cedrus: separate the data direction from the stateful API | the JPEG decoder (0035) | later |
| 0034 | media: v4l2-jpeg: fix the end of DHT parsing and bound jpeg_skip() | JPEG header parsing (0035, also other JPEG drivers) | [prepared](../upstream/v4l2-jpeg/v1/) as 3 patches |
| 0035 | media: cedrus: add a stateful JPEG decoder for the A10/A20 VE | hardware (M)JPEG decoding, node `cedrus-jpeg` (e.g. UVC webcams) | later |
| 0036 | media: cedrus: add a JPEG encoder for the A10/A20 VE | hardware JPEG encoding, node `cedrus-jpeg-enc` | later |
| 0037 | ARM: dts: sun4i: mk802: add the AXP209 PMIC, WiFi power on PH3, drop the codec | MK802: CPU voltage scaling, WiFi of the RTL8188EUS revision | local only (board revision) |
| 0038 | ARM: dts: sun4i: add the mixer processor | 2D engine on the A10 | later, with 0021 |
| 0039 | iio: adc: sun4i-gpadc: wait for a mode change only when the mode changes | every SoC temperature read busy-waited 100 ms (10 % CPU on the A10) | later |
| 0040 | drm/sun4i: hdmi: audio: do not depend on A20 register reset values | HDMI audio on the A10 (fixes Armbian's HDMI audio patch) | Armbian |
| 0041 | drm/sun4i: hdmi: audio: release the DMA channel on destroy | one DMA channel leaked per modeset (Armbian's HDMI audio patch) | Armbian |
| 0042 | drm/lima: do not use the scheduler entity when saving an error task | oops in the GPU timeout handler with `lima.max_error_tasks` | [prepared](../upstream/lima/v1/) |
| 0043 | drm/lima: do not handle a GP MMU fault as an out-of-heap condition | 10 s stall instead of a reset after a GPU page fault | same |
| 0044 | drm/lima: let a faulted core finish its bus transactions before the reset | GPU stays wedged after a page fault (frozen desktop until a reboot) | same |
| 0045 | drm/lima: cancel a pending heap recovery in the timeout handler | race found by review, not seen on hardware | same |
| 0046 | drm/lima: flush the L2 cache and TLB after growing the heap | **the rare GPU page fault** (`gpmmu page fault at 0x...`, Mali hang): root-caused on the A10, very likely the same on the A20 | same |

"Prepared" means a series for the mainline kernel exists in [`../upstream/`](../upstream/) but has
not been sent yet; "later" means it still needs a mainline rebase first.

## Notes

- **0023 and 0024 contain Bootlin's work.** 0023 is the cedrus rework by Paul Kocialkowski
  (Bootlin, branch `cedrus/h264-encoding`, "media: cedrus: Rework the driver in preparation for
  encoding support"), adapted to 6.18; 0024 contains Bootlin's encoder infrastructure and V3s
  H.264 encoder from the same branch, plus the A10/A20 encoder support written for this project.
  Both patch descriptions credit the origin and list the changes against the Bootlin version; the
  copyright headers of the files are kept. The `From:` line names who prepared the 6.18 port, not
  the author of every line.
- **Provenance of the A10/A20 video engine code.** There is no public documentation of the A20
  encoder and JPEG registers. They were worked out from Allwinner's binary libraries (libvencoder,
  libvecore), from Jens Kuske's research and test programs, and from tests on the hardware; the
  code comments say where a setting follows the library ("the A20 blob does ..."). 0024's rate
  control is an integer reimplementation of the quadratic model of the H.264 reference software
  (JM) as Allwinner's A20 encoder library applies it, with its clamps; it was validated off-board
  against a bit-exact model of the library's rate control, and three bugs of the library are
  deliberately not reproduced (see the comment in `cedrus_enc_rc.h`).
- **The video engine patches (0023-0036) were tested only on the A10 and A20.** Armbian builds one
  `current-sunxi` kernel for all 32-bit sunxi boards (A10, A13, A20, H3, ...); on other SoCs the
  reworked driver is untested, so do not install this kernel on such boards and expect their video
  decoding to stay as it was. (0023 also leaves out Armbian's H616/H6 additions, which only matter
  for the 64-bit kernel.)
- 0023-0036 replace the in-tree staging cedrus driver in place (same module name `sunxi-cedrus`,
  same `CONFIG_VIDEO_SUNXI_CEDRUS`). They belong together: the decoder fixes 0028/0029 are written
  against the reworked driver.
- 0016's filter table is `fir_tab_sun4i` from Allwinner's GPL A10/A20 3.4 kernel (linux-sunxi,
  `drivers/video/sunxi/disp/de_be.c`).
- 0017: the budgets (260 Mpixels/s, 0.8 GB/s) were measured at DRAM 432 MHz on one 1080p60 head;
  they do not account for DRAM 384 MHz or for a second head's scanout (see its description).
- 0021: overlapping copies inside one buffer (parameter `allow_overlap`, default off) were verified
  on hardware after the description was written (all scroll directions, 1-pixel and 1-line
  steps, edges); the X server patch uses them for scrolling when the parameter is on. Its other
  parameter, `timeout_ms`, is a debug aid; leave it at 0.
- 0024 has diagnostic module parameters (`a20_rec_wr_2mb`, `a20_mv_canary`, `rc_i_qp_offset`);
  leave them at their defaults.
- 0014 has a known imprecision: a commit without any plane on a CRTC is treated as cursor-only and
  does not wait for the register load. No effect was seen on the hardware; the mainline version in
  `../upstream/drm-sun4i-cursor/` fixes it.
- 0045 differs from the earliest tested version: review added a second interrupt mask after the
  cancel. The kernels tested on both boards contain this version.
- Every patch carries `Assisted-by: Claude:claude-opus-5-5`: the work was done with an AI assistant
  and reviewed and tested on the hardware.
