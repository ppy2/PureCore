#!/bin/sh

rm -f /etc/asound.conf
ln -s /etc/asound.usb /etc/asound.conf
echo USB > /etc/output
/etc/init.d/S01statusmonitor restart
sync
if ls /etc/init.d/S95* >/dev/null 2>&1; then
    /etc/init.d/S95* restart
else
    echo "Service S95* not found, skipping restart"
fi

