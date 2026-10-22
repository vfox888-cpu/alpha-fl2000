# Instructions for AI agents

This repo is a Linux DRM/KMS kernel driver (`alpha_fl2000`) for USB3→HDMI
adapters based on the Fresco Logic FL2000DX (USB ID `1d5c:2000`). If a user
asks you to install it, follow these steps in order and check each result.

## Install

```sh
lsusb | grep -i 1d5c:2000          # adapter present? if not: ask the user to plug it into a USB3 port
lsusb -t | grep -B2 -A2 5000M      # must be on a 5000M (USB3) bus; USB2 ports will not work
mokutil --sb-state                 # note whether Secure Boot is enabled (see below)
sudo ./install.sh                  # DKMS install + modprobe/udev config; needs the user's sudo password
dkms status | grep alpha-fl2000    # expect "installed"
```

`install.sh` installs `dkms`, `build-essential` and kernel headers itself
(apt or dnf). The module is rebuilt automatically on every kernel update.

## Secure Boot

If `mokutil --sb-state` says enabled, DKMS signs the module with a local key
and asks the user to choose a one-time password. Explain to the user that
after reboot a blue "MOK management" screen appears: choose **Enroll MOK →
Continue → Yes**, type that password, reboot. You cannot do this step for
them. Do not suggest disabling Secure Boot unless the user asks.

## After install

Ask the user to reboot, then verify:

```sh
lsmod | grep alpha_fl2000
sudo dmesg | grep -i fl2000        # look for the connector / EDID lines, no errors
sudo sh -c 'cat /sys/bus/usb/drivers/alpha_fl2000/*/status'   # link / mode state
```

If the monitor works but the mouse or windows land on the wrong side, the
monitor order is wrong: tell the user to fix it in Settings → Displays
(drag the screens to match the desk). No code change is needed.

## Rules

- **Never** `rmmod` / `modprobe -r` / reload the module inside a running
  graphical session — it can crash gnome-shell. Always reboot instead.
- Do not edit files under `src/` to "fix" an install problem; report the
  `dmesg` output to the user instead.
- Uninstall: `sudo ./install.sh --remove`, then reboot.

More detail: [README.md](README.md) (Troubleshooting, Module parameters).
