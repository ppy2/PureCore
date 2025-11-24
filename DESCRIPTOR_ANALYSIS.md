# USB Descriptor Differences Analysis

## SUMMARY OF ALL DIFFERENCES (Original vs Gadget)

### ✅ FIXED
1. **iSerialNumber**: 0x03 → 0x00 (S98xingcore: не создавать serialnumber)
2. **String indices**: 0x04-0x0F → 0x00 (f_uac2.c: все = 0)
3. **Device Qualifier Class**: 0xEF/0x02/0x01 → 0x00/0x00/0x00 (composite.c)

### ❌ NOT FIXED - CRITICAL

4. **bNumConfigurations**: 0x01 → **0x02** (нужна вторая конфигурация!)

5. **MISSING Configuration #2**:
   ```
   Configuration Descriptor #2:
   - bConfigurationValue: 0x02
   - wTotalLength: 0x0012 (18 bytes)
   - bNumInterfaces: 1
   - bmAttributes: 0xC0 (Self-powered)
   - MaxPower: 100mA

   Interface:
   - bInterfaceNumber: 0
   - bAlternateSetting: 0
   - bNumEndPoints: 0
   - bInterfaceClass: 0xFF (Vendor specific)
   - bInterfaceSubClass: 0xFF
   - bInterfaceProtocol: 0xFF
   - iInterface: 0x00
   ```

6. **Other Speed Configuration Descriptor**:
   - **Текущий (gadget)**: Копия UAC2 (213 bytes, 2 interfaces, audio)
   - **Должен быть**: Vendor-specific (18 bytes, 1 interface, 0xFF/0xFF/0xFF)
   ```
   Other Speed Configuration Descriptor:
   - bDescriptorType: 0x07
   - wTotalLength: 0x0012 (18 bytes) ← сейчас 0x00D5!
   - bNumInterfaces: 0x01 ← сейчас 0x02!
   - bConfigurationValue: 0x01

   Interface:
   - bInterfaceClass: 0xFF ← сейчас Audio!
   - bInterfaceSubClass: 0xFF
   - bInterfaceProtocol: 0xFF
   - NO endpoints
   ```

---

## DETAILED LINE-BY-LINE COMPARISON

### Device Descriptor
| Field | Original | Gadget | Status |
|-------|----------|--------|--------|
| iSerialNumber | 0x00 | 0x03 | ✅ FIXED |
| bNumConfigurations | **0x02** | 0x01 | ❌ CRITICAL |

### Device Qualifier Descriptor
| Field | Original | Gadget | Status |
|-------|----------|--------|--------|
| bDeviceClass | 0x00 | 0xEF | ✅ FIXED |
| bDeviceSubClass | 0x00 | 0x02 | ✅ FIXED |
| bDeviceProtocol | 0x00 | 0x01 | ✅ FIXED |

### Configuration #1 (UAC2) - String Indices
| Field | Original | Gadget | Status |
|-------|----------|--------|--------|
| IAD iFunction | 0x00 | 0x04 | ✅ FIXED |
| AC Interface iInterface | 0x00 | 0x05 | ✅ FIXED |
| Clock Source iClockSource | 0x00 | 0x07 | ✅ FIXED |
| Input Terminal iTerminal | 0x00 | 0x08 | ✅ FIXED |
| Feature Unit iFeature | 0x00 | 0x0D | ✅ FIXED |
| Output Terminal iTerminal | 0x00 | 0x0B | ✅ FIXED |
| AS Interface #0 iInterface | 0x00 | 0x0E | ✅ FIXED |
| AS Interface #1 iInterface | 0x00 | 0x0F | ✅ FIXED |

### Configuration #2 (MISSING!)
Original has vendor-specific config (18 bytes), gadget doesn't have it at all.

### Other Speed Configuration
| Field | Original | Gadget | Status |
|-------|----------|--------|--------|
| wTotalLength | 0x0012 (18 bytes) | 0x00D5 (213 bytes) | ❌ WRONG |
| bNumInterfaces | 0x01 | 0x02 | ❌ WRONG |
| Interface Class | 0xFF (Vendor) | 0x01 (Audio) | ❌ WRONG |

---

## IMPLEMENTATION PLAN

### Phase 1: Add Configuration #2 (Vendor-specific)

**Option A: configfs (if supported)**
```bash
mkdir -p configs/c.2
echo 0xC0 > configs/c.2/bmAttributes
echo 100 > configs/c.2/MaxPower

# Create vendor-specific function
mkdir -p functions/vendor.0
# Configure as 0xFF/0xFF/0xFF with no endpoints

ln -s functions/vendor.0 configs/c.2/
```

**Option B: Kernel patch (if configfs не поддерживает)**
Modify composite.c to add second configuration programmatically.

### Phase 2: Fix Other Speed Configuration

File: `/opt/PureFox/buildroot/output/build/linux-custom/drivers/usb/gadget/composite.c`

Find "Other Speed Configuration" generation and modify to return vendor-specific 18-byte config instead of mirroring UAC2.

---

## WHY THIS MATTERS

Windows UAC2 driver expects EXACT descriptor match. Differences can cause:
- Device not recognized
- Driver load failure
- Incorrect device enumeration

The vendor-specific Configuration #2 might be used for:
- Firmware updates
- Proprietary control interface
- Fallback mode

Even if unused, Windows may expect it to be present for proper device recognition.
