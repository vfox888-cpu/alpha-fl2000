#!/bin/bash
# Installs alpha_fl2000 into the running kernel's module tree so udev loads it
# at boot whenever a 1d5c:2000 adapter is present, plus the modprobe.d options
# and the udev rule that keep usb-storage off the adapter's built-in flash drive.
# Usage: sudo bash install-autoload.sh        Uninstall: sudo bash install-autoload.sh --remove
set -e
KVER=$(uname -r)
DIR="$(cd "$(dirname "$0")" && pwd)"
SRC=$DIR/src/alpha_fl2000.ko
DST=/lib/modules/$KVER/updates/alpha_fl2000.ko
CONF=/etc/modprobe.d/alpha-fl2000.conf
RULE=/etc/udev/rules.d/71-fl2000-no-storage.rules

if [ "$1" = "--remove" ]; then
	rm -f "$DST" "$CONF" "$RULE"
	depmod -a "$KVER"
	echo "removed: no autoload, no config (a loaded module stays until reboot)"
	exit 0
fi

[ -f "$SRC" ] || { echo "build first: make -C $DIR/src"; exit 1; }
install -D -m 644 "$SRC" "$DST"
install -m 644 "$DIR/modprobe/alpha-fl2000.conf" "$CONF"
install -m 644 "$DIR/udev/71-fl2000-no-storage.rules" "$RULE"
depmod -a "$KVER"
echo "installed: $(modinfo -n alpha_fl2000), $CONF, $RULE"
