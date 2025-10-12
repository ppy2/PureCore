#!/bin/sh

LINUX_DIR=$(ls -d $BUILD_DIR/linux-*)

# Copy DTB to binaries
cp $LINUX_DIR/arch/arm/boot/dts/rv1106_pll.dtb $BINARIES_DIR/

# Create boot.img with zImage and DTB
dd if=/dev/zero of=$BINARIES_DIR/boot.img bs=1 count=0 seek=4194304
dd if=$BINARIES_DIR/zImage of=$BINARIES_DIR/boot.img conv=notrunc
dd if=$BINARIES_DIR/rv1106_pll.dtb of=$BINARIES_DIR/boot.img bs=1 seek=3932160 conv=notrunc

mv -f $BINARIES_DIR/rootfs.ubi $BINARIES_DIR/rootfs.img 2>/dev/null
mv -f $BINARIES_DIR/uboot-env.bin $BINARIES_DIR/env.img 2>/dev/null
rm -f $BINARIES_DIR/*.dtb
