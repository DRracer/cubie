# Series prepared for the mainline kernel

These are the patches of [`../sunxi-6.18/`](../sunxi-6.18/) rebased onto mainline, split and
reworded for the mailing lists. **None of them has been sent yet.** Each directory holds one
series as `git format-patch` output, with the recipients from `get_maintainer.pl` in the cover
letter or in the patch below `---`. The `Signed-off-by:` is added at sending time.

| Directory | Series | Base | From userpatches | State |
|---|---|---|---|---|
| `lima/v1/` | drm/lima: fix a heap-grow page fault and the recovery from MMU faults (5) | drm-misc-next | 0042-0046 | ready |
| `gpadc-hwmon/v1/` | iio: adc: sun4i-gpadc: thermal zone lifetime fix and a hwmon interface (3) | mainline 7.4-rc2+ | 0047-0049 | driver changes tested on the A10 as a 6.18 module; 2/3 build-tested; A20 pending |
| `v4l2-jpeg/v1/` | media: v4l2-jpeg: fix segment length handling in the header parser (3) | v7.3 | 0034 | ready |
| `drm-sun4i-fe-second-pipeline/v1/` | drm/sun4i: backend: route the frontend of a second pipeline | v7.3-rc7 | 0013 | ready; the v7.3-rc7 version build-tested |
| `drm-sun4i-cursor/v1/` | drm/sun4i: backend: expose the hardware cursor as a cursor plane | v7.3-rc7 | 0014 | build-tested; differs from the tested 6.18 version |
| `drm-sun4i-async-flip/v1/` | drm/sun4i: backend: support async page flips (needs the cursor patch) | v7.3-rc7 | 0015 | ready, to be sent after the cursor patch |
| `drm-sun4i-fe-scaler/v1/` | drm/sun4i: frontend: fix tiled crops and downscaling on A10/A20 (3) | v7.3-rc7 | 0004, 0016, 0017 | ready; the 6.18 versions were tested on the A20 (one mode, single head), the v7.3-rc7 versions build-tested |
| `drm-sun4i-vga-rfc/v1/` | [RFC] drm/sun4i: VGA output on the A10/A20 TV encoder (Cubietruck) (3) | v7.3-rc7 | 0010-0012 | RFC (binding, reset and connector questions in the cover letter) |
| `rtc-losc/v1/` | rtc: sunxi: run LOSC from the external 32k crystal (4) | v7.3-rc7 | 0006-0008 | on hold: the crystal stops when the board runs warm |

Not here yet: the MBUS DMA offset fixes (0005, 0009; waiting for a hardware check), the 2D engine
driver (0018-0022, 0038; new uAPI, needs an RFC), the video engine work (0023-0036; builds on
Bootlin's unmerged cedrus rework, needs coordination with its authors) and the gpadc mode-change fix (0039).

Each patch discloses the AI assistance with `Assisted-by: Claude:claude-opus-5-5`, as
`Documentation/process/coding-assistants.rst` asks; the cover letters say it in their text.
