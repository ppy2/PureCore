# AGENTS.md

This file provides guidance to Qoder (qoder.com) when working with code in this repository.

## Project Overview

PureCore is an embedded Linux system for the Luckfox Pico (RV1106 SoC) that emulates PureCore USB Hi-Resolution Audio hardware using USB Audio Class 2.0 (UAC2) gadget mode. The device acts as a USB->I2S audio bridge, receiving high-resolution audio (up to 384kHz/32-bit) via USB and routing it to an I2S DAC.

Target Hardware: Rockchip RV1106 (ARM Cortex-A7), NAND flash (MTD partitions)
Boot Device: `/dev/mtdblock3` (boot partition, 4MB)
Root Filesystem: UBI on `/dev/mtd4`
USB Mode: Device/Gadget (not host)
Network: SSH on port 222 (10.147.20.35), password: purefox

## Build System Commands

### Full System Build
```bash
cd /opt/PureCore/buildroot
make                           # Full build (kernel + rootfs + packages)
make linux-rebuild             # Rebuild kernel only (preserves direct source edits)
make uac2_router-rebuild       # Rebuild UAC2 router application
```

### Flashing to Device
```bash
# Flash boot partition (kernel + DTB) to MTD
cat output/images/boot.img | sshpass -p purefox ssh -p 222 root@10.147.20.35 \
  "cat > /tmp/boot.img && dd if=/tmp/boot.img of=/dev/mtdblock3 bs=1M && sync && reboot"
```

### Flashing with SocToolKit (Alternative Method)

For initial flashing or recovery, you can use the SocToolKit utility:

1. Install SocToolKit on your development machine
2. Connect LuckFox Pico via USB OTG port
3. Power on the device in maskrom mode (hold recovery button during power-on)
4. Use SocToolKit to flash the boot image:
   ```bash
   # Flash kernel and DTB to boot partition
   sudo ./SocToolKit -d /dev/mtdblock3 -f output/images/boot.img
   ```

Critical: The system uses MTD (NAND flash), not MMC/SD card. Boot partition is `/dev/mtdblock3`.

### Kernel Development

Direct source modifications are preserved by `make linux-rebuild`. Modified files:
- `/opt/PureCore/buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_uac2.c` - UAC2 gadget driver
- `/opt/PureCore/buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_vendor.c` - Vendor-specific function (Config 2)
- `/opt/PureCore/buildroot/output/build/linux-custom/drivers/usb/gadget/u_audio.c` - sysfs interface for rate changes

Never use `make linux-dirclean` - it will wipe direct source edits.

## Architecture

### USB Gadget Stack (ConfigFS)

The system uses ConfigFS to configure USB gadget at runtime via `/sys/kernel/config/usb_gadget/purecore/`:

```
purecore/
├── idVendor (0x152A)              # Thesycon (PureCore OEM)
├── idProduct (0x8852)
├── bDeviceClass (0xEF/0x02/0x01)  # Miscellaneous + IAD
├── functions/
│   ├── uac2.usb0/                 # UAC2 Audio function
│   │   ├── p_chmask (0x3)         # Playback: stereo
│   │   ├── p_srate (384000)       # Max sample rate
│   │   ├── p_ssize (4)            # 32-bit samples
│   │   └── ... (capture params)
│   └── vendor.0/                  # Vendor-specific (Config 2) - optional
└── configs/
    ├── c.1/ → uac2.usb0           # Configuration 1: UAC2 Audio
    └── c.2/ → vendor.0            # Configuration 2: Vendor (PureCore compat)
```

Startup script: `/opt/PureCore/ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S98uac2`
Creates gadget configuration on boot. Modify this for USB descriptor changes.

### Audio Routing Architecture

```
Windows ASIO/WASAPI
        │
        ▼ (USB High-Speed)
   UAC2 Gadget (hw:1,0)
        │
        ▼ (kobject_uevent on rate change)
   uac2_router daemon
        │
        ▼ (ALSA copy)
   I2S DAC (hw:0,0)
```

Key component: `uac2_router` - userspace daemon that routes audio from UAC2 gadget (`hw:1,0`) to I2S DAC (`hw:0,0`).

Uevent-based design: No polling! Uses netlink socket to receive `kobject_uevent` from `u_audio.c` kernel driver when sample rate changes.

Fixed format: Always outputs 32-bit I2S (`SND_PCM_FORMAT_S32_LE`) regardless of UAC2 format.

### Sysfs Interface for Rate Monitoring

Custom sysfs interface in `u_audio.c`:
```
/sys/class/u_audio/uac_card1/
├── rate       # Dynamic - triggers kobject_uevent on change
├── format     # Static - read once at startup
└── channels   # Static - read once at startup
```

## File Structure

```
/opt/PureCore/
├── buildroot/                          # Buildroot build system
│   ├── output/build/linux-custom/      # Kernel source (modify directly)
│   │   └── drivers/usb/gadget/
│   │       ├── function/
│   │       │   ├── f_uac2.c            # UAC2 driver (string descriptor mods)
│   │       │   └── f_vendor.c          # Vendor function (Config 2)
│   │       └── u_audio.c               # Sysfs + kobject_uevent
│   └── output/images/
│       ├── boot.img                    # Kernel + DTB (flash to /dev/mtdblock3)
│       └── rootfs.ubi                  # Root filesystem
├── ext_tree/                           # Custom board support
│   ├── board/luckfox/
│   │   ├── config/linux.config         # Kernel configuration
│   │   ├── dts_max/rv1106_pll.dts      # Device tree
│   │   ├── rootfs_overlay/             # Files copied to rootfs
│   │   │   └── etc/init.d/S98uac2      # USB gadget startup script
│   │   └── scripts/post-build.sh       # Creates boot.img
│   └── package/uac2_router/            # Audio routing daemon
│       ├── uac2_router.c               # Main source
│       ├── uac2_router.mk              # Build configuration
│       └── README.md                   # Architecture documentation
```

## Development Workflow

### Modifying USB Descriptors

1. Edit ConfigFS script: `ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S98uac2`
2. Rebuild and flash:
```bash
make
cat output/images/boot.img | sshpass -p purefox ssh -p 222 root@10.147.20.35 \
  "cat > /tmp/boot.img && dd if=/tmp/boot.img of=/dev/mtdblock3 bs=1M && sync && reboot"
```

### Modifying Kernel Driver

1. Edit source directly: `buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_uac2.c`
2. Important: Use `make linux-rebuild`, not `make linux-dirclean`
3. Flash boot.img and reboot
4. Check kernel version updated: `sshpass -p purefox ssh -p 222 root@10.147.20.35 "uname -a"`

### Modifying uac2_router

1. Edit: `ext_tree/package/uac2_router/uac2_router.c`
2. Rebuild: `make uac2_router-rebuild`
3. Binary automatically copied to rootfs
4. Full build required: `make` (creates new rootfs.ubi)
5. Or copy manually:
```bash
scp output/target/usr/bin/uac2_router root@10.147.20.35:/usr/bin/
```