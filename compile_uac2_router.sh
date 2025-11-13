#!/bin/sh

# Compile UAC2 router daemon
# Run this on the build host (not on target device)

set -e

# Cross-compiler settings
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
CC="${CROSS_COMPILE}gcc"
SYSROOT="/opt/PureFox/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot"

echo "Compiling uac2_router with $CC"

$CC -O2 -Wall \
    --sysroot="$SYSROOT" \
    -I"$SYSROOT/usr/include" \
    -L"$SYSROOT/usr/lib" \
    -o /opt/PureFox/ext_tree/board/luckfox/overlay/root/uac2_router \
    /opt/PureFox/ext_tree/board/luckfox/overlay/root/uac2_router.c \
    -lasound -lpthread

echo "Compilation successful: /opt/PureFox/ext_tree/board/luckfox/overlay/root/uac2_router"
chmod +x /opt/PureFox/ext_tree/board/luckfox/overlay/root/uac2_router
