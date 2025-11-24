# AGENTS.md

This file provides guidance to Qoder (qoder.com) when working with code in this repository.

## Project Overview

This is a Buildroot-based embedded Linux system for the Luckfox Pico Max board, focused on creating a USB Audio Class 2 (UAC2) gadget that emulates the XingCore hardware device. The primary goal is to make the device recognized as a high-resolution audio device by Windows and other operating systems.

## Key Components

1. **Buildroot System**: Custom Buildroot configuration for Luckfox Pico Max
2. **USB Gadget Framework**: Implements UAC2 audio device via ConfigFS
3. **Kernel Drivers**: Modified USB gadget drivers (f_uac2.c, f_vendor.c)
4. **Init Scripts**: System startup scripts for configuring USB gadgets
5. **Audio Applications**: Packages for audio streaming (squeezelite, librespot, tidal-connect, etc.)

## Common Commands

### Building the System
```bash
# Full build
./build.sh

# Rebuild just the kernel
cd /opt/PureFox/buildroot && make linux-rebuild
cd /opt/PureFox/buildroot && make

# Clean build
# make clean
```

### Testing and Debugging
#```bash
# Check USB gadget status
#cd /sys/kernel/config/usb_gadget/xingcore
#ls -la

# View USB descriptors
#lsusb -v

# Check kernel messages
#dmesg | grep -i "usb\|gadget\|uac"

# Restart USB gadget
#/etc/init.d/S98xingcore restart
#```

## Code Architecture

### Directory Structure
- `buildroot/`: Main Buildroot source tree
- `ext_tree/`: External Buildroot tree with custom configurations
  - `board/luckfox/`: Board-specific configurations
  - `package/`: Custom packages for audio applications
  - `patches/`: Kernel and U-Boot patches

### Key Files
- `ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S98xingcore`: Main USB gadget configuration script
- `buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_uac2.c`: UAC2 kernel driver
- `buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_vendor.c`: Vendor interface driver

### USB Gadget Configuration
The system implements a dual-configuration USB device:
1. Configuration 1: UAC2 Audio interface
2. Configuration 2: Vendor-specific interface (for Windows compatibility)

## Development Notes

### Making Changes to USB Descriptors
1. Modify `/sys/kernel/config/usb_gadget/xingcore` via ConfigFS for runtime testing
2. Update `ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S98xingcore` for permanent changes
3. For kernel driver changes, modify files in `buildroot/output/build/linux-custom/drivers/usb/gadget/function/`
4. After kernel changes, rebuild with `make linux-rebuild`

### Adding New Audio Applications
1. Create new package directory in `ext_tree/package/`
2. Add package definition files (.mk, Config.in)
3. Reference package in `ext_tree/Config.in`
4. Enable in `ext_tree/configs/luckfox_pico_max_defconfig`