#!/bin/sh

rm -f /etc/asound.conf
ln -s /etc/asound.std /etc/asound.conf
sed -i 's/^SUBMODE=.*$/SUBMODE=std/' /etc/i2s.conf
echo I2S > /etc/output
/etc/init.d/S01statusmonitor restart
sync
if ls /etc/init.d/S95* >/dev/null 2>&1; then
    /etc/init.d/S95* restart
else
    echo "Service S95* not found, skipping restart"
fi