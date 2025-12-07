#!/bin/sh

# Call RV1106 optimizations
if [ -f "$(dirname $0)/optimize-rv1106.sh" ]; then
    echo "Running RV1106 optimizations..."
    cd $BASE_DIR
    $(dirname $0)/optimize-rv1106.sh
    cd $BINARIES_DIR
fi

# Move standard files
mv -f $BINARIES_DIR/rootfs.squashfs $BINARIES_DIR/rootfs.img 2>/dev/null
mv -f $BINARIES_DIR/uboot-env.bin $BINARIES_DIR/env.img 2>/dev/null

# Create optimized boot.img with kernel and DTB
if [ -f "$BINARIES_DIR/zImage" ] && [ -f "$BINARIES_DIR/rv1106.dtb" ]; then
    echo "Creating optimized boot.img..."
    # Sizes: kernel (3.5MB) + DTB (68KB) = 3600000 (0x36F240)
    dd if="$BINARIES_DIR/zImage" of="$BINARIES_DIR/boot.img" bs=3.5M count=1 conv=notrunc 2>/dev/null
    dd if="$BINARIES_DIR/rv1106.dtb" of="$BINARIES_DIR/boot.img" bs=68K seek=45 count=1 conv=notrunc 2>/dev/null
    chmod 644 "$BINARIES_DIR/boot.img"
fi

# Create optimized env.img with fast boot settings
ENV_FILE="$BINARIES_DIR/uboot-env-optimized.txt"
cat > "$ENV_FILE" << 'EOF'
mtdparts=spi-nand0:262144(env),262144@262144(idblock),524288(uboot),4194304(boot),125829120(rootfs)
bootargs=root=/dev/mtdblock4 rootfstype=squashfs ro rootwait console=ttyS0,115200n8 noinitrd init=/linuxrc quiet loglevel=1 fastboot loglevel=1 consolelevel=1 printk.time=0
bootdelay=0
bootcmd=mtd read boot 0x00008000 0 0x360000; mtd read boot 0x00c00000 0x003C0000 0x12000; bootz 0x00008000 - 0x00c00000
EOF

# Simple approach: Re-create squashfs with cleaned files
SQUASHFS_FILE="$BINARIES_DIR/rootfs.squashfs"
if [ ! -f "$SQUASHFS_FILE" ]; then
    # squashfs was already renamed to rootfs.img
    SQUASHFS_FILE="$BINARIES_DIR/rootfs.img"
fi
if [ -f "$SQUASHFS_FILE" ]; then
    echo "Recreating squashfs without www-data..."

    # Create temporary mount point
    TEMP_MOUNT=$(mktemp -d)
    TEMP_TARGET=$(mktemp -d)

    # Mount existing squashfs and copy files
    if mount -t squashfs -o loop "$SQUASHFS_FILE" "$TEMP_MOUNT" 2>/dev/null; then
        echo "Mount successful, copying files..."

        # Copy to temp location
        cp -a "$TEMP_MOUNT"/* "$TEMP_TARGET/"

        # Clean users and groups
        if [ -f "$TEMP_TARGET/etc/passwd" ]; then
            echo "Cleaning passwd..."
            grep -v "^www-data:" "$TEMP_TARGET/etc/passwd" > "$TEMP_TARGET/etc/passwd.tmp"
            mv "$TEMP_TARGET/etc/passwd.tmp" "$TEMP_TARGET/etc/passwd"
        fi

        if [ -f "$TEMP_TARGET/etc/group" ]; then
            echo "Cleaning groups..."
            grep -v "^www-data:" "$TEMP_TARGET/etc/group" | grep -v "^tty:" > "$TEMP_TARGET/etc/group.tmp"
            mv "$TEMP_TARGET/etc/group.tmp" "$TEMP_TARGET/etc/group"
        fi

        # Unmount
        umount "$TEMP_MOUNT"

        # Create clean squashfs
        echo "Creating clean squashfs..."
        mksquashfs "$TEMP_TARGET" "$SQUASHFS_FILE" -noappend -processors 25 -b 128K -Xhc -comp lz4

        # Cleanup
        rm -rf "$TEMP_TARGET" "$TEMP_MOUNT"

        echo "Clean squashfs created successfully!"
    else
        echo "Failed to mount squashfs"
        rm -rf "$TEMP_TARGET" "$TEMP_MOUNT"
    fi
else
    echo "Squashfs file not found: $SQUASHFS_FILE"
fi

# Replace env.img with optimized version
if command -v mkenvimage >/dev/null 2>&1; then
    echo "Creating optimized env.img..."
    mkenvimage -s 0x40000 -o "$BINARIES_DIR/env.img" "$ENV_FILE" 2>/dev/null
elif [ -f "$HOST_DIR/bin/mkenvimage" ]; then
    echo "Creating optimized env.img..."
    $HOST_DIR/bin/mkenvimage -s 0x40000 -o "$BINARIES_DIR/env.img" "$ENV_FILE" 2>/dev/null
fi

rm -f $BINARIES_DIR/*.dtb
rm -f $BINARIES_DIR/uboot-env-optimized.txt
rm -f $BINARIES_DIR/env-opt.img
