#!/bin/sh

set -ve

export LINUX_DIR=`ls -d output/build/linux-main`

cp $LINUX_DIR/arch/arm/boot/dts/rv1106_ext.dtb $TARGET_DIR/data/boot/1024_ext.dtb
cp $LINUX_DIR/arch/arm/boot/dts/rv1106_pll.dtb $TARGET_DIR/data/boot/1024_pll.dtb
cp $LINUX_DIR/arch/arm/boot/dts/rv1106_512_ext.dtb $TARGET_DIR/data/boot/512_ext.dtb

rm -f $TARGET_DIR/etc/init.d/*shairport-sync
rm -f $TARGET_DIR/etc/init.d/*upmpdcli
rm -f $TARGET_DIR/etc/init.d/*urandom
rm -f $TARGET_DIR/etc/init.d/*mpd
#rm -f $TARGET_DIR/etc/init.d/*mdev
rm -f -r $TARGET_DIR/etc/alsa
#rm -f -r $(TARGET_DIR/var/db
echo "uprclautostart = 1" > $TARGET_DIR/etc/upmpdcli.conf
echo "friendlyname = PureOS" >> $TARGET_DIR/etc/upmpdcli.conf
#sed -i "s/console::respawn/#console::respawn/g" $TARGET_DIR/etc/inittab
sed -i "s/#PermitRootLogin prohibit-password/PermitRootLogin yes/g" $TARGET_DIR/etc/ssh/sshd_config
chown root:root $TARGET_DIR/usr/bin/php-cgi
chmod u+s $TARGET_DIR/usr/bin/php-cgi
wget https://curl.se/ca/cacert.pem -O $TARGET_DIR/etc/ssl/certs/ca-certificates.crt

# Add www-data to audio group for ALSA access without sudo
sed -i 's/^audio:x:29:upmpdcli$/audio:x:29:upmpdcli,www-data/' $TARGET_DIR/etc/group

# Remove GDB Python helper files (only needed for debugging, they break strip)
find $TARGET_DIR -name "*-gdb.py" -delete

# Create SquashFS for Tidal libraries (MAX only - save rootfs space)
if [ -d "$TARGET_DIR/usr/lib/tidal" ] && [ ! -f "$TARGET_DIR/usr/lib/tidal.sqfs" ]; then
    echo "Creating SquashFS image for Tidal..."
    mksquashfs $TARGET_DIR/usr/lib/tidal $TARGET_DIR/usr/lib/tidal.sqfs -comp xz -b 256K -noappend
    echo "Removing original Tidal directory from rootfs..."
    rm -f $TARGET_DIR/usr/lib/tidal/*
fi





