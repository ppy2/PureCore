# PureCore - USB Hi-Resolution Audio Interface

Embedded Linux system for Luckfox Pico PRO/MAX (RV1106) that emulates hardware interface UAC2 to I2S.

## Hardware
- **Target**: Rockchip RV1106 (ARM Cortex-A7)
- **USB Mode**: Device/Gadget (USB->I2S audio bridge)
- **Audio Output**: I2S DAC
- **Max Sample Rate**: PCM 768 kHz/32-bit and DSD 512
- **Clock Source**: Internal PLL clocking providing audiophile-grade timing quality comparable to high-end crystal oscillators

## Precision Clocking

The project utilizes the internal PLL of the processor for audio clock generation. This built-in PLL allows achieving sound quality at the level of quality hardware quartz generators, providing precise timing essential for high-resolution audio playback.

## Flashing Instructions

### Flashing with SSH (Normal Operation)
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

<img width="644" height="712" alt="image" src="https://github.com/user-attachments/assets/83a82f51-19bb-4a88-975b-d6e05b4e3b74" />


**Supported Modes:**

| Mode | Driver | Windows API | Formats | Status |
|------|--------|-------------|---------|--------|
| **Without Drivers** | Windows USB Audio 2.0 (built-in) | WASAPI | PCM 44.1-768 kHz | ✅ Working |
| **With custom Drivers** | libusbk (in development) | WASAPI/ASIO | PCM + DSD64-512 | ✅ Testing needed |
| Linux Host | ALSA (snd-usb-audio) | ALSA | PCM + DSD64-512 | ✅ Working |

**Features:**
- ✅ Works without driver installation (Windows 10+)
- ✅ Standard WASAPI support for PCM
- ✅ Linux native DSD via Alt Setting 2 (kernel quirk: QUIRK_FLAG_DSD_RAW)
- ✅ Thesycon ASIO DSD support (requires testing)
- ✅ Volume controls enabled (PCM compatible)
- ✅ Universal compatibility across all platforms

**Limitations:**
- ⚠️ Windows without drivers: PCM only (ignores Alt Setting 2 with DSD)
---