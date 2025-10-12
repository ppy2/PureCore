#!/bin/sh

set -ve

MAINDIR=`pwd`

export LINUX_DIR=`ls -d output/build/linux-main`

cp $LINUX_DIR/arch/arm/boot/dts/rv1106_pll.dtb $BINARIES_DIR/
cp $LINUX_DIR/arch/arm/boot/dts/rv1106_ext.dtb $TARGET_DIR/data/boot/1024_ext.dtb
cp $LINUX_DIR/arch/arm/boot/dts/rv1106_pll.dtb $TARGET_DIR/data/boot/1024_pll.dtb
cp $LINUX_DIR/arch/arm/boot/dts/rv1106_512_ext.dtb $TARGET_DIR/data/boot/512_ext.dtb

cd $BINARIES_DIR
# 1. Create empty boot.img file 4MB in size
dd if=/dev/zero of=boot.img bs=1 count=0 seek=4194304
sleep 1
# 2. Write zImage from the beginning of file
dd if=zImage of=boot.img conv=notrunc
sleep 1
# 3. Write rv1106_pll.dtb at offset 0x003C0000 (3932160 bytes)
dd if=rv1106_pll.dtb of=boot.img bs=1 seek=3932160 conv=notrunc
sleep 1 
#rm -f $BINARIES_DIR/*.dtb
#rm -f $BINARIES_DIR/zImage
cd $MAINDIR

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

# Compress all large binaries and libraries with UPX (MAX only - save rootfs space)
#echo "Compressing binaries and libraries with UPX..."
#find $TARGET_DIR/usr/bin -type f -size +100k -executable -exec upx --best --lzma {} \; 2>/dev/null || true
#find $TARGET_DIR/usr/sbin -type f -size +100k -executable -exec upx --best --lzma {} \; 2>/dev/null || true
#find $TARGET_DIR/usr/lib -type f -size +100k -name "*.so*" -exec upx --best --lzma {} \; 2>/dev/null || true

# Create SquashFS for Tidal libraries (MAX only - save rootfs space)
if [ -d "$TARGET_DIR/usr/lib/tidal" ] && [ ! -f "$TARGET_DIR/usr/lib/tidal.sqfs" ]; then
    echo "Creating SquashFS image for Tidal..."
    mksquashfs $TARGET_DIR/usr/lib/tidal $TARGET_DIR/usr/lib/tidal.sqfs -comp xz -b 256K -noappend
    
    echo "Removing original Tidal directory from rootfs..."
    rm -rf $TARGET_DIR/usr/lib/tidal
    mkdir -p $TARGET_DIR/usr/lib/tidal
fi




