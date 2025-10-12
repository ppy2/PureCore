#!/bin/sh

rm -f /etc/asound.conf
ln -s /etc/asound.lr /etc/asound.conf
sed -i 's/^SUBMODE=.*$/SUBMODE=lr/' /etc/i2s.conf
sync

