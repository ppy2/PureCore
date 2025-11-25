#!/bin/sh

rm -f /etc/asound.conf
ln -s /etc/asound.8ch /etc/asound.conf
sed -i 's/^SUBMODE=.*$/SUBMODE=8ch/' /etc/i2s.conf
sync
