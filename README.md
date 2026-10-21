# alpha-fl2000

Custom Linux DRM/KMS driver for the **Fresco Logic FL2000DX** USB3→HDMI adapter
chip (USB ID `1d5c:2000`), paired with an **ITE IT66121** HDMI transmitter.
Written from scratch against register-level behavior, not a fork of an
existing driver — see [Credits](#credits--references) for the prior work
consulted along the way.

Unlike the legacy fbdev-style FL2000 drivers floating around, this is a
proper atomic **DRM/KMS** driver: GNOME/Wayland (and any other DRM-aware
compositor) sees the adapter as a real GPU/output, not a framebuffer device.

## Hardware tested

- Fresco Logic FL2000DX (`1d5c:2000`) — tested on a Hagibis USB-A/USB-C → HDMI
  adapter
- ITE IT66121 HDMI transmitter on I2C address `0x4c` (bridge address is
  auto-detected, `0x4d` also probed)
- Monitor: Xiaomi "Mi Monitor", 1920×1080 @ 50–75 Hz
- Host: AMD Ryzen + AMD iGPU, kernel 7.0, direct USB3 (SuperSpeed) port

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

## Build

```sh
cd src
make
```

Needs the kernel headers for the running kernel (`KDIR` defaults to
`/lib/modules/$(uname -r)/build`).

## Install (autoload at boot)

```sh
make -C src
sudo bash install-autoload.sh          # uninstall: sudo bash install-autoload.sh --remove
sudo reboot
```

The installer copies the module to `/lib/modules/$(uname -r)/updates/`, runs
`depmod`, and installs:

- [`modprobe/alpha-fl2000.conf`](modprobe/alpha-fl2000.conf) — keeps
  `usb-storage` off the adapter's built-in 8 MB flash drive (polling it caused
  USB resets);
- [`udev/71-fl2000-no-storage.rules`](udev/71-fl2000-no-storage.rules) — same,
  for the case where `usb-storage` is loaded from the initramfs.

udev then loads the driver whenever the adapter is present, before the
desktop starts. Loading it *before* GNOME matters: mutter handles a GPU that
appears at startup much better than one hot-added into a running session.

After a kernel update the module must be rebuilt and reinstalled (no DKMS yet).
If a broken build ever prevents booting, add `module_blacklist=alpha_fl2000`
to the kernel command line in GRUB.

Manual one-off load for testing: `sudo insmod src/alpha_fl2000.ko`.

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

Not done yet: DKMS packaging, audio, modes above 1080p60, E-DDC (EDID beyond
two blocks).

## Credits / references

Written from scratch, but register-level behavior was cross-checked against:

- [FrescoLogic/FL2000](https://github.com/FrescoLogic/FL2000) (GPL-2.0) —
  vendor's own reference driver
- [klogg/fl2000_drm](https://github.com/klogg/fl2000_drm) (GPL-2.0) —
  community DRM driver for the same chip

## License

GPL-2.0, see [LICENSE](LICENSE).
