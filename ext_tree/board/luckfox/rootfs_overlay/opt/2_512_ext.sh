#!/bin/sh

sed -i 's/007c003c/007c001c/' /etc/init.d/S94ioi2s
sed -i 's/MODE=pll/MODE=ext/' /etc/i2s.conf
sed -i 's/MCLK=1024/MCLK=512/' /etc/i2s.conf

flash_erase /dev/mtd3 0x003C0000 0x2
sleep 1
nandwrite -p /dev/mtd3 -s 0x003C0000 /data/boot/512_ext.dtb

sync
#reboot -f



