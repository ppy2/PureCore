#!/bin/sh

rm -f /etc/asound.conf
ln -s /etc/asound.plr /etc/asound.conf
sed -i 's/^SUBMODE=.*$/SUBMODE=plr/' /etc/i2s.conf
sync

