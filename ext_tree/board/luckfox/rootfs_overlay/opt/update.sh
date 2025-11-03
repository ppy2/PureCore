#!/bin/sh

SCRIPT_PATH="/opt/update.sh"
SCRIPT_NEW="/tmp/update.sh.new"
UPDATE_SERVER="luckfox@luckfox.puredsd.ru::luckfox/opt/update.sh"

# Check if script update is needed (only on first run, not on restart)
if [ "$UPDATE_SELF_DONE" != "1" ]; then
    echo "Checking for update.sh updates..."
    
    # Download new version of update.sh to temp location
    sshpass -p 'luckfox' rsync -av "$UPDATE_SERVER" "$SCRIPT_NEW" 2>/dev/null
    
    if [ $? -eq 0 ] && [ -f "$SCRIPT_NEW" ]; then
        # Compare checksums
        OLD_MD5=$(md5sum "$SCRIPT_PATH" | awk '{print $1}')
        NEW_MD5=$(md5sum "$SCRIPT_NEW" | awk '{print $1}')
        
        if [ "$OLD_MD5" != "$NEW_MD5" ]; then
            echo "New version of update.sh found, updating..."
            chmod +x "$SCRIPT_NEW"
            cp "$SCRIPT_NEW" "$SCRIPT_PATH"
            rm -f "$SCRIPT_NEW"
            
            echo "Restarting with new update.sh..."
            export UPDATE_SELF_DONE=1
            exec "$SCRIPT_PATH" "$@"
        else
            echo "update.sh is up to date"
            rm -f "$SCRIPT_NEW"
        fi
    else
        echo "Could not check for updates, continuing..."
    fi
fi

/etc/init.d/S95* stop
killall status_monitor
rm -f /data/*.img

sshpass -p 'luckfox' rsync -acv --delete-before --one-file-system \
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








