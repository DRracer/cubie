# Building the kernel with Armbian

You need a Linux PC with Docker (Armbian's build framework runs in a container) and about 30 GB of
free disk space. The patches are against Linux 6.18.56 (the configs pin that tag) and were built
with the Armbian build framework at commit `ddae2880450452b61373d17daf52ad6d30ba5f66` (trunk,
11 October 2026, version 26.11.0-trunk; sunxi kernel 6.18, u-boot v2026.07). Newer revisions may
have moved on; start from that commit.

| File | What |
|---|---|
| `userpatches/config-cubie-a20.conf` | A20 kernel (tested on a Cubietruck): pins v6.18.56, Cortex-A7 code (0003), `SERIAL_8250_16550A_VARIANTS` (Cubietruck Bluetooth UART), `DRM_SUN4I_MP=m` (2D engine), step_wise thermal governor |
| `userpatches/config-cubie-a10.conf` | A10 kernel (MK802): pins v6.18.56, Cortex-A8 code, uniprocessor, 2G/2G split, `DRM_SUN4I_MP=m`, step_wise thermal governor |
| `userpatches/u-boot/v2026.07-sunxi/board_cubietruck/` | optional: Cubietruck DRAM at 432 MHz instead of Armbian's 384 MHz (see [`../boards/cubietruck/README.md`](../boards/cubietruck/README.md)) |

The kernel patches themselves are in [`../kernel/sunxi-6.18/`](../kernel/sunxi-6.18/) (index with
what each patch is needed for). The tested kernels had all of 0001-0046; you can leave out patches
you do not want, see the index (notably 0001/0002, which loosen DRM access for the VDPAU player
path).

```sh
git clone https://github.com/armbian/build armbian-build
cd armbian-build
git checkout ddae2880450452b61373d17daf52ad6d30ba5f66
C=/path/to/cubie
mkdir -p userpatches/kernel/archive/sunxi-6.18
cp $C/kernel/sunxi-6.18/*.patch userpatches/kernel/archive/sunxi-6.18/
cp $C/armbian/userpatches/config-*.conf userpatches/
./compile.sh cubie-a20 kernel         # A20; for the A10: ./compile.sh cubie-a10 kernel
```

The two configs build different kernels (Cortex-A7 SMP vs. Cortex-A8 uniprocessor), but both end
up as the same Armbian package name, `linux-image-current-sunxi`: build one, collect its packages,
then build the other. The A20 kernel does not boot on an A10.

The result is in `output/debs/`: `linux-image-current-sunxi_*.deb`,
`linux-dtb-current-sunxi_*.deb` and `linux-headers-current-sunxi_*.deb`. The patched device trees
(VGA, 2D engine, RTC crystal, MK802 PMIC) are in the dtb package. Start from Armbian's Trixie
image for your board on an SD card. The new packages replace the installed kernel; keep a copy of
`/boot` (or the old packages: `apt download linux-image-current-sunxi linux-dtb-current-sunxi`
before installing) so you can go back:

```sh
sudo cp -a /boot /boot.orig
sudo dpkg -i ./linux-image-current-sunxi_*.deb ./linux-dtb-current-sunxi_*.deb ./linux-headers-current-sunxi_*.deb
sudo apt-mark hold linux-image-current-sunxi linux-dtb-current-sunxi linux-headers-current-sunxi
sudo reboot
```

The hold keeps Armbian's updates from replacing the patched kernel. **It also stops kernel
security fixes:** the patches are pinned to 6.18.56. Newer 6.18.y releases will mostly take them
with little or no change; rebuild from time to time (change the tag in the config) and check that
all patches still apply. (`dpkg -i` instead of `apt install` because a rebuilt package can carry
the same version as the installed one.) If you keep any modules in `/lib/modules/*/updates/`, run
`sudo depmod -a` once the new kernel is running (see "Pitfall: out-of-tree modules" in the
Cubietruck guide).

I installed kernel packages on existing Armbian Trixie images; building a complete image with
the same configs (`./compile.sh cubie-a20 build BOARD=cubietruck RELEASE=trixie BUILD_MINIMAL=yes
BUILD_DESKTOP=no`) should work but was not tested.

Then continue with the board guide: [Cubietruck](../boards/cubietruck/README.md),
[MK802](../boards/mk802/README.md).
