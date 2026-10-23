# alpha-fl2000

Custom Linux DRM/KMS driver for the **Fresco Logic FL2000DX** USB3→HDMI adapter
chip (USB ID `1d5c:2000`), paired with an **ITE IT66121** HDMI transmitter.
Written from scratch against register-level behavior, not a fork of an
existing driver — see [Credits](#credits--references) for the prior work
consulted along the way.

Unlike the legacy fbdev-style FL2000 drivers floating around, this is a
proper atomic **DRM/KMS** driver: GNOME/Wayland (and any other DRM-aware
compositor) sees the adapter as a real GPU/output, not a framebuffer device.

> [!WARNING]
> **Do not unplug the adapter while you are logged in to GNOME.** If you do,
> **reboot (or log out and back in) right after plugging it back.**
> GNOME (Mutter) does not release a removed USB GPU: after an unplug it keeps
> a stale entry for it, and from then on the USB monitor stays dark after the
> screen wakes from sleep/blanking, and a replugged adapter is not picked up
> (`Failed to hotplug secondary gpu ... device already present`). This is a
> Mutter limitation, not something the driver can fix. As long as the adapter
> stays plugged in, sleep/wake works normally.

## Hardware tested

- Fresco Logic FL2000DX (`1d5c:2000`) — tested on a Hagibis USB-A/USB-C → HDMI
  adapter
- ITE IT66121 HDMI transmitter on I2C address `0x4c` (bridge address is
  auto-detected, `0x4d` also probed)
- Monitor: Xiaomi "Mi Monitor", 1920×1080 @ 50–75 Hz
- Hosts (direct USB3 SuperSpeed port in both cases):
  - AMD Ryzen + AMD iGPU, kernel 7.0
  - Lenovo ideapad 330-15IKB: Intel Core i3-7020U + Intel HD Graphics 620
    (Kaby Lake-U GT2), Ubuntu 26.04.1 LTS, kernel 7.0.0-38-generic,
    GNOME 50.1 on Wayland, Secure Boot off. Installed via DKMS
    (`install.sh`); after reboot the module autoloads, binds `1d5c:2000` at
    5000M instead of `usb-storage`, detects IT66121 @ `0x4c`, the output
    shows up as `card1-HDMI-A-1: connected` and runs 1920×1080@60. Works with
    the Intel iGPU as the primary GPU (occasional 35–42 ms full-frame updates
    show up in `dmesg`)

USB 2.0 ports do not provide enough bandwidth for 1080p raw pixel data — a
direct USB3 port is required.

## Features

- Atomic DRM/KMS pipeline (plane/CRTC/encoder/connector), GEM-shmem backed —
  no legacy fbdev hacks
- EDID read directly over FL2000's I2C engine (see
  [Known quirks](#known-quirks))
- Fast format conversion to the FL2000 wire format (RGB888 / RGB565,
  byte-swapped nibbles), driven by DRM damage-clip tracking so unchanged
  regions aren't reprocessed every commit
- Dedicated hardware ARGB32 cursor plane — the pointer moves independently of
  full-frame updates instead of being baked into the frame
- Refuses imported GPU/VRAM dma-bufs (`gem_prime_import` returns
  `-EOPNOTSUPP` for them) so the compositor falls back to copying via its own
  GPU into a buffer in system RAM — CPU readback straight from VRAM measured
  30–40× slower than from RAM on this hardware
- Separate `sg_table` per in-flight URB (a shared scatterlist across URBs
  triggered AMD IOMMU faults)
- `/sys/bus/usb/drivers/alpha_fl2000/*/status` — live counters: updates/sec,
  average pixels per update, time per update, FL2000 frame counter, IT66121
  HPD/RxSense/video-lock bits

## Known quirks

- **Hot-unplug in a running GNOME session is not handled by Mutter.** After the
  adapter is unplugged, gnome-shell keeps the removed GPU in its list. The next
  wake from screen blanking then never re-enables the USB output (the driver
  never receives the modeset — no `режим …` line in `dmesg`), and replugging
  fails with `Failed to hotplug secondary gpu '/dev/dri/cardN': device already
  present` / `No available CRTC for monitor`. Only a fresh session (reboot or
  log out/in) clears it. Replugging also gives the connector a new name
  (e.g. `HDMI-1` → `HDMI-3`), so a saved layout in `~/.config/monitors.xml`
  may not match until the next reboot.
- The IT66121's own DDC engine can't be used for EDID on this board: its DDC
  pins share the same I2C bus as FL2000's own DDC/HDCP engine, and the two
  arbitrate for the bus and lose. EDID is instead read directly through
  FL2000's I2C engine at the standard EDID address `0x50`.
- There is no real vblank interrupt over USB. The CRTC state is marked
  `no_vblank = true`; DRM generates page-flip completion events itself.
- **Do not reload the module in a live GNOME session.** `rmmod` blocks while
  mutter still holds GEM/dma-buf imports, and a hot module swap has been
  observed to crash gnome-shell outright (`SIGSEGV` in
  `meta_monitor_manager_rebuild`). Reload only after a logout or reboot.
- fwupd polling the adapter's built-in 8 MB virtual flash drive can cause
  spurious USB resets while the adapter is plugged in.

## Quick install (Ubuntu / Debian)

1. Plug the adapter into a **USB3 port directly on the computer** (blue port /
   "SS" marking; not through a USB2 hub).
2. Run:

```sh
sudo apt install -y git
git clone https://github.com/vfox888-cpu/alpha-fl2000.git
cd alpha-fl2000
sudo ./install.sh
sudo reboot
```

`install.sh` installs `dkms`, compiler and kernel headers if missing, registers
the driver with **DKMS** (it is rebuilt automatically after every kernel
update), and installs the config below. After the reboot the monitor appears
in *Settings → Displays*; arrange it there like any other screen.

Uninstall: `sudo ./install.sh --remove && sudo reboot`.

Secure Boot: if it is enabled, unsigned modules are rejected. Either disable
Secure Boot, or enroll the DKMS signing key when `dkms` asks for a MOK
password during install and confirm it in the blue MOK screen on the next boot.

### Check that it works

```sh
lsmod | grep alpha_fl2000                     # driver loaded
sudo dmesg | grep -i alpha_fl2000 | tail      # probe messages, no errors
cat /sys/class/drm/card*-HDMI-A-*/status      # "connected" for the adapter output
sudo cat /sys/bus/usb/drivers/alpha_fl2000/*/status   # vid_stable 1, frame counter growing
```

### Troubleshooting

| Symptom | Fix |
|---|---|
| "No signal" on the monitor | Use a direct USB3 port; check `status` for `vid_stable 1`; replug the HDMI cable |
| Monitor not in Displays | Reboot (the driver must load before the desktop starts) |
| USB monitor stays dark after the screen wakes up, or after replugging the adapter | **Reboot** (or log out and back in). Happens only if the adapter was unplugged during the session — see the warning at the top |
| Cursor stops at the screen edge | Order the monitors in *Settings → Displays* the way they stand on the desk |
| Stopped working after a kernel update | `sudo dkms autoinstall` then reboot; make sure `linux-headers-$(uname -r)` is installed |
| System does not boot after an update | In GRUB press `e`, add `module_blacklist=alpha_fl2000` to the `linux` line, boot, then `sudo ./install.sh --remove` |

## For AI assistants

If you are an AI agent helping a user install this driver, run these steps in
order and check each result before continuing:

```sh
lsusb | grep -i 1d5c:2000                     # adapter present; if missing, ask the user to replug into USB3
lsusb -t | grep -B2 -A2 5000M                 # adapter should sit on a 5000M (USB3) bus
mokutil --sb-state                            # if "SecureBoot enabled", explain the MOK enrollment step to the user
git clone https://github.com/vfox888-cpu/alpha-fl2000.git && cd alpha-fl2000
sudo ./install.sh                             # needs the user's sudo password
dkms status | grep alpha-fl2000               # expect "installed"
```

Then ask the user to reboot and, after reboot, run the checks from
[Check that it works](#check-that-it-works). Never `rmmod`/reload the module
inside a running GNOME session — it can crash gnome-shell; always reboot instead.

## Manual build and install (without DKMS)

```sh
make -C src
sudo bash install-autoload.sh          # uninstall: sudo bash install-autoload.sh --remove
sudo reboot
```

This copies the module to `/lib/modules/$(uname -r)/updates/` (must be redone
after every kernel update) and installs the same config:

- [`modprobe/alpha-fl2000.conf`](modprobe/alpha-fl2000.conf) — keeps
  `usb-storage` off the adapter's built-in 8 MB flash drive (polling it caused
  USB resets);
- [`udev/71-fl2000-no-storage.rules`](udev/71-fl2000-no-storage.rules) — same,
  for the case where `usb-storage` is loaded from the initramfs.

Loading the driver *before* GNOME matters: mutter handles a GPU that appears at
startup much better than one hot-added into a running session. For a one-off
test without installing: `sudo insmod src/alpha_fl2000.ko`.

### Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `max_mbytes` | 0 (from link speed) | USB bandwidth budget for video, MB/s; low values force RGB565 |
| `max_mem_mb` | 128 | Cap on RAM used by dumb framebuffers (0 = no cap) |
| `no_foreign_import` | off | Refuse dma-buf imports from other GPUs. Smoother, but with mutter 50 on AMD closing the laptop lid made mutter allocate ~3 GB of VRAM/GTT and run the system out of memory — leave off |

### Diagnostics

- `/sys/bus/usb/drivers/alpha_fl2000/*/status` (root) — link, bridge lock
  (`vid_stable`), HPD, frame counters, slow/unaligned row reads.
- `/sys/bus/usb/drivers/alpha_fl2000/*/mem` — framebuffer memory in use / peak / cap.
- Updates slower than 30 ms are logged to `dmesg`.

Log messages and code comments are partly in Russian.

## Status

Working on the reference host (Ryzen 7 laptop, AMD iGPU, Ubuntu, kernel 7.0,
GNOME 50 / Wayland): 1920×1080@60 RGB888 over USB3, extends the desktop next
to the laptop's own HDMI output, hardware cursor plane, EDID autodetect,
damage-clip partial updates with 16-byte-aligned streaming reads from
imported GPU buffers (~2–5 ms per full frame), survives USB resets without
re-creating the DRM device, retries the HDMI bridge lock, forces a hotplug
event once HPD rises so the compositor picks the monitor up.

Not done yet: audio, modes above 1080p60, E-DDC (EDID beyond
two blocks).

## Credits / references

Written from scratch, but register-level behavior was cross-checked against:

- [FrescoLogic/FL2000](https://github.com/FrescoLogic/FL2000) (GPL-2.0) —
  vendor's own reference driver
- [klogg/fl2000_drm](https://github.com/klogg/fl2000_drm) (GPL-2.0) —
  community DRM driver for the same chip

## License

GPL-2.0, see [LICENSE](LICENSE).
