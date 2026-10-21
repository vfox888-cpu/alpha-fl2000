#!/bin/bash
# Ставит alpha_fl2000 в модули ядра: udev сам грузит его при загрузке, когда видит адаптер 1d5c:2000.
# Запуск: pkexec bash install-autoload.sh        Откат: pkexec bash install-autoload.sh --remove
set -e
KVER=$(uname -r)
SRC=/home/v/Documents/fl2000_driver/src/alpha_fl2000.ko
DST=/lib/modules/$KVER/updates/alpha_fl2000.ko

if [ "$1" = "--remove" ]; then
	rm -f "$DST"
	depmod -a "$KVER"
	echo "удалено: автозагрузки больше нет (загруженный модуль уйдёт после перезагрузки)"
	exit 0
fi

install -D -m 644 "$SRC" "$DST"
depmod -a "$KVER"
echo "установлено: $(modinfo -n alpha_fl2000)"
