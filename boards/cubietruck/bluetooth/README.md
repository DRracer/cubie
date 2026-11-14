# Cubietruck Bluetooth bring-up (AP6210) and RTC crystal

## What happens
The AP6210 module gets its 32 kHz low-power clock from the A20's LOSC oscillator (via CLK_OUT_A). The Bluetooth
controller samples that clock **only when it is powered up**. If LOSC runs from the A20's internal RC oscillator at that
moment, the controller answers every command, but it cannot scan or pair (no inquiry or LE scan results) until it is
powered up again with the crystal running. A controller powered up on the crystal keeps working afterwards.

Mainline leaves LOSC on the RC oscillator; kernel patches 0006-0008 (`../../../kernel/sunxi-6.18/`) select the 32.768 kHz crystal. On the test board the
crystal also **stops when the A20 runs warm**: SoC sensor about 43.5 °C under sustained CPU/GPU load (40-44 °C at the
moment it stops). The RTC then falls back to the RC oscillator by itself (LOSC_CTRL bit 13 set) until the board has
cooled down (about 41 °C). With a fan blowing on the board, the worst load test kept it at about 37 °C and the crystal
kept running. Whether other Cubietrucks have the same margin is not known.

So only a Bluetooth power-up on a hot board matters: at boot, or when the driver is re-bound.

## Files
| File | Install to | What |
|---|---|---|
| `cubietruck-bt.conf` | `/etc/modprobe.d/` | blacklists `hci_uart`, so udev does not power the module up at an arbitrary moment |
| `cubietruck-bt` | `/usr/local/sbin/` (executable) | loads `hci_uart` once the crystal has run cleanly for 10 s (up to 120 s, then anyway), watches the crystal through the power-up, re-binds on setup failures; after an unclean power-up it re-initialises Bluetooth by itself once the crystal runs and no device is connected. `cubietruck-bt rebind` does a clean re-initialisation on demand (e.g. before pairing) |
| `cubietruck-bt.service` | `/etc/systemd/system/` | runs it at boot |
| `a20-losc-kick` + `.service` | as above | optional, RTC only: restarts a stopped crystal (rewrites LOSC_CTRL 30 s after it stopped, then with growing pauses); the crystal is usually back 10 s after a kick once the load is gone |

```sh
sudo install -m644 cubietruck-bt.conf /etc/modprobe.d/
sudo install -m755 cubietruck-bt a20-losc-kick /usr/local/sbin/
sudo install -m644 cubietruck-bt.service a20-losc-kick.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable cubietruck-bt a20-losc-kick
```
Needs python3, bluez (`hciconfig`, `bluetoothctl`) and access to `/dev/mem` (the services run as root).

## If Bluetooth cannot find or pair devices
`journalctl -u cubietruck-bt -p warning` says whether the module was powered up without the crystal. Let the board
cool down (stop heavy jobs, or a fan); the service re-initialises Bluetooth once the crystal runs and nothing is
connected. `sudo cubietruck-bt rebind` does it right away.

Tested on one Cubietruck (A20, 2 GiB) with kernel 6.18.56 and the kernel patches of this repository.
