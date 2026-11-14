# Device-tree overlays

Install each with Armbian's tool, which compiles it into `/boot/overlay-user/` and adds it to
`user_overlays=` in `/boot/armbianEnv.txt` (needs the package `device-tree-compiler`):

```sh
sudo armbian-add-overlay sun7i-a20-cma-224m.dts
```

| Overlay | Board | What |
|---|---|---|
| `sun7i-a20-cma-224m.dts` | A20, 2 GiB (Cubietruck) | CMA pool 96 -> 224 MiB inside the video engine's 256 MiB window. **Needs `bootm_size=0x20000000` in `armbianEnv.txt`** |
| `sun4i-a10-cma-160m.dts` | A10, 1 GiB (MK802) | CMA pool 96 -> 160 MiB, same window rule, same `bootm_size` |
| `sun7i-a20-cubietruck-bt.dts` | Cubietruck | Bluetooth UART at 115200 baud and a Bluetooth address. **Edit `local-bd-address` first** (all zeros is ignored) |
| `sun7i-a20-cpu-1008.dts` | A20 (Cubietruck) | optional 1008 MHz @ 1.45 V operating point (the vendor 3.4 configuration's highest entry, above mainline's 1.40 V maximum); watch the temperature, a fan is advised |

Details: [`../boards/cubietruck/README.md`](../boards/cubietruck/README.md#overlays) and
[`../boards/mk802/README.md`](../boards/mk802/README.md).
