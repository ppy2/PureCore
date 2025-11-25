#!/bin/sh

# mdev passes variables via environment:
# $ACTION = add/remove
# $MDEV = device name (e.g. card1)

# Debug logging
echo "mdev hotplug: ACTION=$ACTION DEVICE=$MDEV" >> /tmp/usb-hotplug.log

# Skip internal I2S card
[ "$MDEV" = "card0" ] && exit 0

case "$ACTION" in
    add)
        # USB audio device connected
        echo "USB audio device connected: $MDEV" >> /tmp/usb-hotplug.log
        sleep 1
        # Stop then start audio services (cleaner than restart)
        /etc/init.d/S01statusmonitor restart
        for service in /etc/init.d/S95*; do
            if [ -x "$service" ]; then
                "$service" stop
                "$service" start &
            fi
        done
        ;;
    remove)
        # USB audio device disconnected
        echo "USB audio device disconnected: $MDEV" >> /tmp/usb-hotplug.log

        # Stop audio services to prevent 100% CPU usage (especially librespot)
        # They will be restarted when USB device is reconnected
        for service in /etc/init.d/S95*; do
            [ -x "$service" ] && "$service" stop &
        done
        ;;
esac