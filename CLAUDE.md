# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**PureFox** is an embedded Linux system for the Luckfox Pico (RV1106 SoC) that emulates XingCore USB Hi-Resolution Audio hardware using USB Audio Class 2.0 (UAC2) gadget mode. The device acts as a USB->I2S audio bridge, receiving high-resolution audio (up to 384kHz/32-bit) via USB and routing it to an I2S DAC.

**Target Hardware**: Rockchip RV1106 (ARM Cortex-A7), NAND flash (MTD partitions)
**Boot Device**: `/dev/mtdblock3` (boot partition, 4MB)
**Root Filesystem**: UBI on `/dev/mtd4`
**USB Mode**: Device/Gadget (not host)
**Network**: SSH on port 222 (10.147.20.35), password: purefox

## Build System Commands

### Full System Build
```bash
cd /opt/PureFox/buildroot
make                           # Full build (kernel + rootfs + packages)
make linux-rebuild             # Rebuild kernel only (preserves direct source edits)
make uac2_router-rebuild       # Rebuild UAC2 router application
```

### Flashing to Device
```bash
# Flash boot partition (kernel + DTB) to MTD
cat output/images/boot.img | sshpass -p purefox ssh -p 222 root@10.147.20.35 \
  "cat > /tmp/boot.img && dd if=/tmp/boot.img of=/dev/mtdblock3 bs=1M && sync && reboot"

# Flash full rootfs (not recommended - use boot.img for kernel updates)
# UBI image: output/images/rootfs.ubi
```

**Critical**: The system uses **MTD (NAND flash)**, not MMC/SD card. Boot partition is `/dev/mtdblock3`, not `/dev/mmcblk0p5`.

### Kernel Development

**Direct source modifications** are preserved by `make linux-rebuild`. Modified files:
- `/opt/PureFox/buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_uac2.c` - UAC2 gadget driver
- `/opt/PureFox/buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_vendor.c` - Vendor-specific function (Config 2)
- `/opt/PureFox/buildroot/output/build/linux-custom/drivers/usb/gadget/u_audio.c` - sysfs interface for rate changes

**Never use** `make linux-dirclean` - it will wipe direct source edits.

Kernel config: `/opt/PureFox/ext_tree/board/luckfox/config/linux.config`

### Testing USB Descriptors

```bash
# On device - check ConfigFS setup
sshpass -p purefox ssh -p 222 root@10.147.20.35 "
  ls /sys/kernel/config/usb_gadget/xingcore/functions/
  cat /sys/kernel/config/usb_gadget/xingcore/UDC
  dmesg | grep -i 'xingcore\|vendor'
"

# On Windows - capture descriptors with USB Device Tree Viewer
# Save to original2.txt for comparison
```

## Architecture

### USB Gadget Stack (ConfigFS)

The system uses **ConfigFS** to configure USB gadget at runtime via `/sys/kernel/config/usb_gadget/xingcore/`:

```
xingcore/
├── idVendor (0x152A)              # Thesycon (XingCore OEM)
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
    └── c.2/ → vendor.0            # Configuration 2: Vendor (XingCore compat)
```

**Startup script**: `/opt/PureFox/ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S98xingcore`
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

**Key component**: `uac2_router` - userspace daemon that routes audio from UAC2 gadget (`hw:1,0`) to I2S DAC (`hw:0,0`).

**Uevent-based design**: No polling! Uses netlink socket to receive `kobject_uevent` from `u_audio.c` kernel driver when sample rate changes. See `/opt/PureFox/ext_tree/package/uac2_router/README.md` for details.

**Fixed format**: Always outputs 32-bit I2S (`SND_PCM_FORMAT_S32_LE`) regardless of UAC2 format, matching original XingCore hardware behavior.

### Sysfs Interface for Rate Monitoring

Custom sysfs interface in `u_audio.c`:
```
/sys/class/u_audio/uac_card1/
├── rate       # Dynamic - triggers kobject_uevent on change
├── format     # Static - read once at startup
└── channels   # Static - read once at startup
```

When Windows changes sample rate, kernel driver:
1. Updates `rate` sysfs attribute
2. Sends `kobject_uevent(KOBJ_CHANGE)` via netlink
3. `uac2_router` receives event instantly (no polling)
4. Router reconfigures ALSA with new rate

## Critical Kernel Modifications

### 1. f_uac2.c - String Descriptors

**Location**: `buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_uac2.c:1100-1122`

Original XingCore has **all interface string descriptors = 0x00** (empty). For exact emulation:
```c
iad_desc.iFunction = 0;
std_ac_if_desc.iInterface = 0;
// ... all other iInterface/iClockSource/iTerminal = 0
```

**Current status**: Strings are **enabled** (standard UAC2) for Windows compatibility. XingCore emulation with empty strings causes `Code 10` errors in Windows without proprietary driver.

### 2. f_vendor.c - Vendor-Specific Function

**Location**: `buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_vendor.c`

Implements Configuration 2 (vendor-specific interface 0xFF/0xFF/0xFF) for XingCore compatibility. **Critical requirement**: Must call `config_group_init_type_name()` in `vendor_alloc_inst()` for ConfigFS registration.

```c
static struct usb_function_instance *vendor_alloc_inst(void) {
    struct usb_function_instance *fi;
    fi = kzalloc(sizeof(*fi), GFP_KERNEL);
    fi->free_func_inst = vendor_free_instance;

    // CRITICAL: ConfigFS registration
    config_group_init_type_name(&fi->group, "", &vendor_func_type);
    return fi;
}
```

Without this, `mkdir functions/vendor.0` fails with "Function not implemented".

### 3. u_audio.c - Sysfs + Uevent

Adds `/sys/class/u_audio/` with kobject_uevent on rate changes. See `uac2_router/README.md`.

## Device Tree & Clock Configuration

**DTB**: `rv1106_pll.dtb` - PLL mode configuration for UAC2 audio
**Location**: `ext_tree/board/luckfox/dts_max/rv1106_pll.dts`

The PLL configuration ensures stable audio clocks for high sample rates (up to 384kHz).

## Common Issues

### "Составное USB устройство" Code 10 in Windows

**Symptom**: Device shows as "Composite USB Device" with error "STATUS_DEVICE_DATA_ERROR (Code 10)"

**Root cause**: XingCore requires proprietary Windows driver. Without it, standard Windows UAC2 driver works **only if**:
1. Device Class = 0x00/0x00/0x00 (not 0xEF/0x02/0x01)
2. Only Configuration 1 (UAC2), no Configuration 2 (vendor)
3. String descriptors populated (not all 0x00)

**Workaround for driverless operation**:
```bash
# On device - remove vendor config, change to standard UAC2
sshpass -p purefox ssh -p 222 root@10.147.20.35 "
  cd /sys/kernel/config/usb_gadget/xingcore
  echo '' > UDC
  rm configs/c.2/vendor.0 2>/dev/null
  rmdir configs/c.2 2>/dev/null
  echo 0x00 > bDeviceClass
  echo 0x00 > bDeviceSubClass
  echo 0x00 > bDeviceProtocol
  echo ffb00000.usb > UDC
"
```

**Note**: This breaks XingCore driver compatibility but enables Windows native UAC2 support.

### Gadget Not Starting

Check dmesg for errors:
```bash
sshpass -p purefox ssh -p 222 root@10.147.20.35 "dmesg | grep -i 'gadget\|xingcore\|udc'"
```

Common errors:
- `Config c/2 needs at least one function` - vendor.0 not created (f_vendor.c not compiled or missing ConfigFS init)
- `failed to start xingcore: -22` - Invalid descriptor configuration
- `couldn't find an available UDC` - USB controller not initialized

### uac2_router Not Finding UAC Card

```bash
# Check sysfs interface exists
ls /sys/class/u_audio/
# Should show: uac_card0 or uac_card1

# If missing, check u_audio.c modifications in kernel
dmesg | grep u_audio
```

## File Structure

```
/opt/PureFox/
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
│   │   │   └── etc/init.d/S98xingcore  # USB gadget startup script
│   │   └── scripts/post-build.sh       # Creates boot.img
│   └── package/uac2_router/            # Audio routing daemon
│       ├── uac2_router.c               # Main source
│       └── README.md                   # Architecture documentation
└── original.txt 			# USB descriptor dumps for comparison
```

## Development Workflow

### Modifying USB Descriptors

1. Edit ConfigFS script: `ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S98xingcore`
2. Rebuild and flash:
```bash
make
cat output/images/boot.img | sshpass -p purefox ssh -p 222 root@10.147.20.35 \
  "cat > /tmp/boot.img && dd if=/tmp/boot.img of=/dev/mtdblock3 bs=1M && sync && reboot"
```
3. Test on Windows, capture descriptors with USB Device Tree Viewer
4. Compare with `original.txt` to verify emulation accuracy

### Modifying Kernel Driver

1. Edit source directly: `buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_uac2.c`
2. **Important**: Use `make linux-rebuild`, **not** `make linux-dirclean`
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

## XingCore Emulation Status

**Goal**: Bit-perfect emulation of XingCore USB Hi-Resolution Audio hardware descriptors for Windows driver compatibility.

**Achieved**:
- ✅ VID/PID: 0x152A/0x8852
- ✅ Device Class: 0xEF/0x02/0x01 (Miscellaneous + IAD)
- ✅ Configuration 1: UAC2 Audio (213 bytes, identical HexDump)
- ✅ Configuration 2: Vendor-specific 0xFF/0xFF/0xFF (18 bytes, identical HexDump)
- ✅ Device Qualifier descriptor
- ✅ Self-powered, 100mA

**Outstanding Issue**:
- ❌ Windows Code 10 error without XingCore proprietary driver
- ❌ Descriptors are identical, but Windows requires signed driver for vendor-specific devices

**Workaround**: Remove Configuration 2 and use Device Class 0x00/0x00/0x00 for native Windows UAC2 support (breaks XingCore driver compatibility).

## Resources

- **UAC2 Router Architecture**: `ext_tree/package/uac2_router/README.md`
- **Buildroot Documentation**: `buildroot/docs/manual/`
- **USB Gadget ConfigFS**: https://www.kernel.org/doc/Documentation/usb/gadget_configfs.txt
- **Device Access**: `ssh -p 222 root@10.147.20.35` (password: purefox)
