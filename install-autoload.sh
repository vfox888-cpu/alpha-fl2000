#!/bin/bash
# Ставит alpha_fl2000 в модули ядра: udev сам грузит его при загрузке, когда видит адаптер 1d5c:2000.
# Заодно настройки: modprobe.d (параметры модуля, без встроенного диска адаптера) и правило udev.
# Запуск: pkexec bash install-autoload.sh        Откат: pkexec bash install-autoload.sh --remove
set -e
KVER=$(uname -r)
DIR=/home/v/Documents/fl2000_driver
SRC=$DIR/src/alpha_fl2000.ko
DST=/lib/modules/$KVER/updates/alpha_fl2000.ko
CONF=/etc/modprobe.d/alpha-fl2000.conf
RULE=/etc/udev/rules.d/71-fl2000-no-storage.rules

if [ "$1" = "--remove" ]; then
	rm -f "$DST" "$CONF" "$RULE"
	depmod -a "$KVER"
	echo "удалено: автозагрузки и настроек больше нет (загруженный модуль уйдёт после перезагрузки)"
	exit 0
fi

install -D -m 644 "$SRC" "$DST"
install -m 644 "$DIR/modprobe/alpha-fl2000.conf" "$CONF"
install -m 644 "$DIR/udev/71-fl2000-no-storage.rules" "$RULE"
depmod -a "$KVER"
echo "установлено: $(modinfo -n alpha_fl2000), $CONF, $RULE"
