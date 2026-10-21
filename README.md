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

## Load

```sh
sudo modprobe drm_kms_helper drm_shmem_helper drm_client_lib
sudo insmod src/alpha_fl2000.ko
```

Optional module parameter:

- `max_mbytes` — cap the USB bandwidth budget used for video, in MB/s
  (default: 80% of the negotiated USB link speed). Useful for throttling
  down to RGB565 on a slow/shared bus.

A udev rule granting non-root access to the device is in
[`udev/70-fl2000.rules`](udev/70-fl2000.rules) — copy it to
`/etc/udev/rules.d/` if you need the device node writable without root.

## Status

Working: 1920×1080@60 RGB888 over USB3 on bare metal, extends the desktop as
a second monitor under GNOME, hardware cursor, EDID autodetect, damage-clip
partial updates (~5 ms/full-frame conversion on the reference host).

Under investigation: the connector is occasionally not picked up by the
compositor again after an unrelated compositor restart (e.g. gnome-shell
being killed and respawned by the OOM killer) — looks like stale DRM-master
state on the compositor side rather than a driver bug, not yet root-caused.

## Credits / references

Written from scratch, but register-level behavior was cross-checked against:

- [FrescoLogic/FL2000](https://github.com/FrescoLogic/FL2000) (GPL-2.0) —
  vendor's own reference driver
- [klogg/fl2000_drm](https://github.com/klogg/fl2000_drm) (GPL-2.0) —
  community DRM driver for the same chip

## License

GPL-2.0, see [LICENSE](LICENSE).
