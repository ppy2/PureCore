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

<img width="400" height="600" alt="image" src="https://github.com/user-attachments/assets/83a82f51-19bb-4a88-975b-d6e05b4e3b74" />


**Supported Modes:**

| Mode | Driver | Windows API | Formats | Status |
|------|--------|-------------|---------|--------|
| **Without Drivers** | Windows USB Audio 2.0 (built-in) | WASAPI | PCM 44.1-768 kHz | ✅ Working |
| **With custom Drivers** | libusbk (in development) | WASAPI/ASIO | PCM + DSD64-512 | 🚧 In development |
| Linux Host | ALSA (snd-usb-audio) | ALSA | PCM + DSD64-512 | ✅ Working |

**Features:**
- ✅ Works without driver installation (Windows 10+)
- ✅ Standard WASAPI support for PCM
- ✅ Linux native DSD via Alt Setting 2 (kernel quirk: QUIRK_FLAG_DSD_RAW)
- ✅ Thesycon ASIO DSD support
- 🚧 Volume controls enabled (PCM compatible)
- ✅ Universal compatibility across all platforms

**Limitations:**
- ⚠️ Windows without drivers: PCM only (ignores Alt Setting 2 with DSD)

## Flashing the Device

Luckfox Pico Pro/Max comes preloaded with a factory test image. Users must manually flash the OS to onboard Flash.

### Prerequisites
1. **Install USB Drivers**: [RK DriverAssistant](https://files.luckfox.com/wiki/Omni3576/TOOLS/DriverAssitant_v5.13.zip)
2. **Download Programming Tool**:
   - Windows: [SocToolKit](https://files.luckfox.com/wiki/Luckfox-Pico/Software/SocToolKit_v1.98_20240705_01_win.zip)
   - Linux: [upgrade_tool](/assets/files/upgrade_tool_v2.17-bfd48dcdba9fd8013872ca2abff19a8d.zip)

### Windows Flashing to SPI NAND Flash

1. **Install USB Drivers**

   <img width="400" alt="Driver Installation" src="https://wiki.luckfox.com/img/RV1106/Luckfox-Pico-RKDriver.png">

2. **Open SocToolKit as Administrator**

   <img width="400" alt="SocToolKit Interface" src="https://wiki.luckfox.com/img/RV1106/Flash/SocToolKit-RV1106.png">

3. **Connect Device in Mask ROM Mode**
   Hold BOOT button while connecting to PC

4. **Select Firmware and Flash**
   Browse to `buildroot/output/images/boot.img` and click Download

   <img width="400" alt="Flashing Process" src="https://wiki.luckfox.com/img/RV1106/Flash/Pico-Pi-W-download.png">

### Linux Flashing to SPI NAND Flash

1. **Check Device Connection**
   ```bash
   lsusb
   ```

   <img width="400" alt="Linux USB Detection" src="https://wiki.luckfox.com/img/RV1106/Flash/Linux-lsusb.png">

2. **Flash Firmware**
   ```bash
   sudo upgrade_tool uf buildroot/output/images/boot.img
   ```

   <img width="400" alt="Linux Flashing" src="https://wiki.luckfox.com/img/RV1106/KVM/upgrade.png">

### Notes
- The PureCore build generates `boot.img` in `buildroot/output/images/`
- This contains both kernel and device tree for NAND flash boot
- Ensure device is in mask ROM mode (BOOT button held) for flashing

## Credits

Special thanks to Vladimir Aleev (https://github.com/aleev/ale-linux-rv1106) for the initial idea and implementation of I2S on rv1106.

---
