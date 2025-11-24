# PureFox - PureCore USB Hi-Resolution Audio Emulator

Embedded Linux system for Luckfox Pico (RV1106) that emulates PureCore USB Audio hardware using USB Audio Class 2.0 (UAC2).

## Hardware
- **Target**: Rockchip RV1106 (ARM Cortex-A7)
- **USB Mode**: Device/Gadget (USB->I2S audio bridge)
- **Audio Output**: I2S DAC
- **Max Sample Rate**: 768 kHz / 32-bit

**Supported Modes:**

| Mode | Driver | Windows API | Formats | Status |
|------|--------|-------------|---------|--------|
| **Without Drivers** | Windows USB Audio 2.0 (built-in) | WASAPI | PCM 44.1-768 kHz | ✅ Working |
| **With Thesycon Drivers** | PureCore ASIO | WASAPI/ASIO | PCM + DSD64-512 | ✅ Testing needed |
| Linux Host | ALSA (snd-usb-audio) | ALSA | PCM + DSD64-512 | ✅ Working |

**DSD Support:**
- DSD64: 2.8224 MHz
- DSD128: 5.6448 MHz
- DSD256: 11.2896 MHz
- DSD512: 22.5792 MHz

**Features:**
- ✅ Works without driver installation (Windows 10+)
- ✅ Standard WASAPI support for PCM
- ✅ Linux native DSD via Alt Setting 2 (kernel quirk: QUIRK_FLAG_DSD_RAW)
- ✅ Thesycon ASIO DSD support (requires testing)
- ✅ Volume controls enabled (PCM compatible)
- ✅ Universal compatibility across all platforms

**Limitations:**
- ⚠️ Windows without drivers: PCM only (ignores Alt Setting 2 with DSD)
- ⚠️ Thesycon DSD support needs verification

---

### Windows Driver Installation

**Without Drivers (pcm-standard):**
1. Connect device to Windows PC
2. Windows will automatically install "USB Audio 2.0" driver
3. Device appears as "PureCore USB Hi-Resolution Audio"
4. Use WASAPI-compatible applications (Foobar2000, MusicBee, etc.)

**With Thesycon Drivers:**
1. Install PureCore ASIO drivers
2. Connect device to Windows PC
3. Use ASIO-compatible applications (Roon, Foobar2000 ASIO, etc.)

**Note:** If device shows "PureCore" with libwdi/WinUSB driver, manually switch to "USB Audio 2.0" driver in Device Manager.

---

