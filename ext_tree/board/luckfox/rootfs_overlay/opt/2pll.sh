#!/bin/sh

sed -i 's/007c001c/007c003c/' /etc/init.d/S94ioi2s
sed -i 's/MODE=ext/MODE=pll/' /etc/i2s.conf
sed -i 's/MCLK=512/MCLK=1024/' /etc/i2s.conf

flash_erase /dev/mtd3 0x003C0000 0x2
sleep 1
nandwrite -p /dev/mtd3 -s 0x003C0000 /data/boot/1024_pll.dtb

sync
#reboot -f
