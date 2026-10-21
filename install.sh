#!/bin/bash
# One-step installer: registers the driver with DKMS (rebuilt automatically on
# every kernel update), installs the modprobe/udev config, loads at boot.
# Usage: sudo ./install.sh            Uninstall: sudo ./install.sh --remove
set -e
NAME=alpha-fl2000
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$(dirname "$0")/dkms.conf")
DIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR=/usr/src/$NAME-$VER
CONF=/etc/modprobe.d/alpha-fl2000.conf
RULE=/etc/udev/rules.d/71-fl2000-no-storage.rules

[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0 $*"; exit 1; }

if [ "$1" = "--remove" ]; then
	dkms remove -m $NAME -v "$VER" --all 2>/dev/null || true
	rm -rf "$SRCDIR" "$CONF" "$RULE"
	rm -f /lib/modules/*/updates/alpha_fl2000.ko
	depmod -a
	echo "removed. Reboot to unload the driver."
	exit 0
fi

if ! command -v dkms >/dev/null || [ ! -d "/lib/modules/$(uname -r)/build" ]; then
	if command -v apt-get >/dev/null; then
		apt-get install -y dkms build-essential "linux-headers-$(uname -r)"
	elif command -v dnf >/dev/null; then
		dnf install -y dkms kernel-devel make gcc
	else
		echo "install dkms, make, gcc and your kernel headers, then rerun"; exit 1
	fi
fi

# Older manual installs (install-autoload.sh) would shadow the DKMS module.
rm -f "/lib/modules/$(uname -r)/updates/alpha_fl2000.ko"
dkms remove -m $NAME -v "$VER" --all 2>/dev/null || true
rm -rf "$SRCDIR"
mkdir -p "$SRCDIR"
cp -r "$DIR/dkms.conf" "$DIR/src" "$SRCDIR/"
make -C "$SRCDIR/src" clean >/dev/null 2>&1 || true
dkms add -m $NAME -v "$VER"
dkms install -m $NAME -v "$VER"

install -m 644 "$DIR/modprobe/alpha-fl2000.conf" "$CONF"
install -m 644 "$DIR/udev/71-fl2000-no-storage.rules" "$RULE"
depmod -a
echo
echo "installed: $(modinfo -n alpha_fl2000)"
echo "Plug the adapter into a USB3 port and reboot."
