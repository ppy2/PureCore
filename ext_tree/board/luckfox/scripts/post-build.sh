#!/bin/sh

set -ve

MAINDIR=`pwd`

export LINUX_DIR=`ls -d output/build/linux-custom`

# Copy kernel and DTB to binaries
cp $LINUX_DIR/arch/arm/boot/zImage $BINARIES_DIR/
cp $LINUX_DIR/arch/arm/boot/dts/rv1106_pll.dtb $BINARIES_DIR/

# Copy DTB to target (PLL mode only for UAC2)
cp $LINUX_DIR/arch/arm/boot/dts/rv1106_pll.dtb $TARGET_DIR/data/boot/1024_pll.dtb

cd $BINARIES_DIR
# Create boot.img with zImage and DTB
dd if=/dev/zero of=boot.img bs=1 count=0 seek=4194304
dd if=zImage of=boot.img conv=notrunc
dd if=rv1106_pll.dtb of=boot.img bs=1 seek=3932160 conv=notrunc
rm zImage
cd $MAINDIR

rm -f $TARGET_DIR/etc/init.d/*urandom
rm -f $TARGET_DIR/etc/init.d/*seedrng
#rm -f $TARGET_DIR/etc/init.d/*mdev
rm -f -r $TARGET_DIR/etc/alsa
#sed -i "s/console::respawn/#console::respawn/g" $TARGET_DIR/etc/inittab
sed -i "s/#PermitRootLogin prohibit-password/PermitRootLogin yes/g" $TARGET_DIR/etc/ssh/sshd_config

# Remove GDB Python helper files (they prevent buildroot's strip from working)
find $TARGET_DIR -name "*-gdb.py" -delete

# Remove www-data user/group (not needed for audio-only system)
if [ -f "$TARGET_DIR/etc/passwd" ]; then
    sed -i '/^www-data:/d' "$TARGET_DIR/etc/passwd" 2>/dev/null || true
fi
if [ -f "$TARGET_DIR/etc/group" ]; then
    sed -i '/^www-data:/d' "$TARGET_DIR/etc/group" 2>/dev/null || true
    sed -i '/^tty:/d' "$TARGET_DIR/etc/group" 2>/dev/null || true
fi

# Strip external toolchain libraries (buildroot's target-finalize runs BEFORE post-build)
# When packages are reinstalled, libraries are copied unstripped, so we strip them here
STRIP_BIN="$HOST_DIR/opt/ext-toolchain/bin/arm-none-linux-gnueabihf-strip"
if [ -f "$TARGET_DIR/lib/libstdc++.so.6.0.33" ] && file "$TARGET_DIR/lib/libstdc++.so.6.0.33" | grep -q "not stripped"; then
    echo "Stripping external toolchain libraries..."
    find $TARGET_DIR/lib -name "*.so*" -type f -exec $STRIP_BIN {} \; 2>/dev/null || true
fi

# Compress large binaries with UPX (MAX only - save rootfs space)
#if command -v upx >/dev/null 2>&1; then
#    echo "Compressing binaries with UPX..."
#    find $TARGET_DIR/usr/bin -type f -size +500k -executable ! -name "*.so*" -exec upx --best --lzma {} \; 2>/dev/null || true
#    find $TARGET_DIR/usr/sbin -type f -size +500k -executable ! -name "*.so*" -exec upx --best --lzma {} \; 2>/dev/null || true
#    find $TARGET_DIR/usr/ap* -type f -size +500k -executable ! -name "*.so*" -exec upx --best --lzma {} \; 2>/dev/null || true
#fi






