#!/bin/sh


/etc/init.d/S95* stop
killall status_monitor
rm -f /data/*.img

sshpass -p 'luckfox' rsync -acv --delete --size-only --one-file-system \
--exclude=.git \
--exclude=/dev \
--exclude=/proc \
--exclude=/sys \
--exclude=/mnt \
--exclude=/root \
--filter='protect /usr/aprenderer/*.dat' \
--filter='protect /usr/aplayer/*.dat' \
--filter='protect /data/ethaddr.txt' \
--filter='protect /etc/resolv.conf' \
--filter='protect /etc/init.d/S95*' \
--filter='protect /var/www/radio.json' \
luckfox@luckfox.puredsd.ru::luckfox / || exit 1

flash_erase /dev/mtd0 0 0 || exit 1
mtd_debug write /dev/mtd0 0 262144 /data/env.img || exit 1
sleep 2
flash_erase /dev/mtd1 0 0 || exit 1
mtd_debug write /dev/mtd1 0 262144 /data/idblock.img || exit 1
sleep 2
flash_erase /dev/mtd2 0 0 || exit 1
mtd_debug write /dev/mtd2 0 524288 /data/uboot.img || exit 1
sleep 2
flash_erase /dev/mtd3 0 0 || exit 1
mtd_debug write /dev/mtd3 0 4194304 /data/boot.img || exit 1
sleep 2

rm -f /data/*.img

sync








