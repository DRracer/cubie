# Rikomagic MK802 (Allwinner A10, 1 GiB)

The MK802 is an HDMI stick from about 2012: Allwinner A10 (one Cortex-A8 core), 1 GiB RAM,
Mali-400 MP1. The same kernel series as for the A20 boards (0001-0046) runs on it; the patches
that matter only for the A10 are 0037-0041. Tested on one MK802 (the revision with an RTL8188EUS
WiFi chip) with Armbian Trixie, kernel 6.18.56 and all of 0001-0046, LXQt on Xorg.

Building the kernel: [`../../armbian/README.md`](../../armbian/README.md) (A10 config); patch index:
[`../../kernel/sunxi-6.18/README.md`](../../kernel/sunxi-6.18/README.md).

## What works

| Feature | State | Patches / parts |
|---|---|---|
| GPU (Mali-400 MP1, lima, Mesa), Xorg/glamor | works; glmark2 windowed 94 at 1080p60, 97 at 1080p50 (with the 2D engine X server) | 0005, 0009; lima fixes 0042-0046 |
| GPU stability | a GPU page fault that hung the desktop (`gpmmu page fault at 0x45c9000`) is fixed: with many heap grows per frame it hit 5 of 20 test runs before, 0 of 40 after | 0042-0046 |
| Hardware cursor | works | 0014 |
| 2D engine (G2D) in X | works; on a single 1080p head faster than the dual-head Cubietruck (1080p copy ~180 Mpixels/s) | 0018-0022, 0038 + `../../userspace/xserver-g2d/` |
| H.264 hardware decode, zero-copy playback (VDPAU) | works, bit-exact on Big Buck Bunny 1080p | 0001, 0002, 0023-0029 + `../../userspace/vdpau-sun4i/` |
| H.264 hardware encode | works; every encoded stream is bit-identical to the A20's (the A10 VE reports the same version, 0x1623) | 0023-0032 |
| JPEG hardware decode/encode | works; 4096x4096 pictures need a bigger memory pool than the 160 MiB below | 0033-0036 |
| HDMI audio | works | 0040 (the A10 resets the audio registers to other values than the A20), 0041 (DMA channel leak per mode set) |
| SoC temperature | reads without the 100 ms busy wait per read (cost 10 % CPU) | 0039 |
| PMIC (AXP209), CPU voltage scaling, WiFi | works | 0037 |

## Board specifics (patch 0037)

- Mainline's MK802 device tree has no AXP209. The board has one on i2c0; it supplies the CPU
  (DCDC2) and the internal logic (DCDC3). With it as `cpu-supply`, cpufreq can scale the voltage.
  ACIN and VBUS are shorted on the board; the stick is fed through either.
- The WiFi chip of this board revision (RTL8188EUS on the second USB host) is powered from PH3.
  With PH12, as in the vendor configuration of the original RTL8188CUS boards, it never shows up
  on USB. Other MK802 revisions may well need PH12; this part of 0037 is a local patch.
- No SATA, Ethernet, IR, analog audio jack or LEDs; the analog codec is disabled.

## Kernel configuration

The A10 config in `../../armbian/userpatches/` differs from the Cubietruck one:

- `ARMV7_TUNE_CORTEX_A8` (patch 0003): ARMv7-A code with Cortex-A8 scheduling. A kernel built
  with the Cortex-A7 target does not run on the A10.
- `SMP=n` and `VMSPLIT_2G`: a uniprocessor kernel for the single core.
- Without SMP there is no `schedutil` governor; the board runs `ondemand`. Armbian's boot-time
  tuning of `ondemand` kept the CPU at its top frequency most of the time; the kernel's default
  tunables work better here. A sampling rate of 8 ms cost about 7 % CPU; 50 ms is enough.

## Memory pool overlay

[`../../overlays/sun4i-a10-cma-160m.dts`](../../overlays/) enlarges the contiguous memory pool
(video engine, GPU and display buffers) from 96 to 160 MiB and keeps it inside the first 256 MiB
of DRAM, the only part the video engine can reach. Install it with `armbian-add-overlay` and set
`bootm_size=0x20000000` in `/boot/armbianEnv.txt`, otherwise u-boot puts the initrd and the device
tree inside that window and the pool does not fit. Do not use the `cma=` kernel argument instead.

## Heat and display mode

The A10 runs hot: on the test board the SoC sensor reads about 70-80 °C at an idle desktop (the
reading is plausible; the bare chip is too hot to touch). The trip points are 85 °C (passive) and
100 °C (critical). At 1920x1080@60 the picture wobbled at times (DRAM and HDMI bandwidth);
1920x1080@50 is stable and saves 17 % of the scan-out bandwidth.

## Userspace for the Cortex-A8

Programs built for the A20 with `-mcpu=cortex-a7 -mfpu=neon-vfpv4` do not run on the A10; rebuild
them for the A8. Debian's own packages and builds with Debian's default flags run on both. The VDPAU
driver: `make CPUFLAGS="-mcpu=cortex-a8 -mfpu=neon"` (see `../../userspace/vdpau-sun4i/README.md`).

## Wayland

Zero-copy video from the video engine works on Weston 14 with the four patches in
[`../../userspace/wayland/`](../../userspace/wayland/) plus a GStreamer waylandsink stride fix: 1080p25
H.264 in real time without dropped frames, about 26 % CPU. LXQt cannot run on Weston (no
wlr-layer-shell), and on labwc (wlroots 0.18) the tiled video buffers cannot be used without
de-tiling them on the CPU (about 92 % CPU, dropped frames), because wlroots only handles buffers
the GPU can texture. The desktop therefore stays LXQt on Xorg; Wayland is for later.
