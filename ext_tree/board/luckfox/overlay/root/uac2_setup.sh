#!/bin/sh

# USB UAC2 Audio Gadget Setup Script
# Automatically configures rv1106 as USB Audio Class 2.0 device
# Supports PCM 44.1-384kHz, 16-32bit

CONFIGFS="/sys/kernel/config"
GADGET="$CONFIGFS/usb_gadget/g1"

# Load configfs module
modprobe libcomposite

# Create gadget
mkdir -p "$GADGET"
cd "$GADGET"

# USB Device Descriptor
echo 0x1d6b > idVendor  # Linux Foundation
echo 0x0104 > idProduct # Multifunction Composite Gadget
echo 0x0100 > bcdDevice # v1.0.0
echo 0x0200 > bcdUSB    # USB 2.0

# Device Strings
mkdir -p strings/0x409
echo "PureFox" > strings/0x409/manufacturer
echo "USB Audio Interface" > strings/0x409/product
echo "UAC2-001" > strings/0x409/serialnumber

# UAC2 Function Configuration
mkdir -p functions/uac2.0

# Playback (USB Host -> rv1106 -> I2S DAC)
echo 2 > functions/uac2.0/p_chmask         # Stereo
echo 384000 > functions/uac2.0/p_srate     # Max 384kHz
echo 4 > functions/uac2.0/p_ssize          # 32-bit samples

# Capture (disabled for now, can enable for ADC input)
echo 0 > functions/uac2.0/c_chmask         # Disabled

# ALSA device control
echo "UAC2 PCM" > functions/uac2.0/function_name

# Create configuration
mkdir -p configs/c.1
mkdir -p configs/c.1/strings/0x409
echo "UAC2 Audio" > configs/c.1/strings/0x409/configuration
echo 500 > configs/c.1/MaxPower

# Link function to configuration
ln -s functions/uac2.0 configs/c.1/

# Enable gadget by binding to UDC
UDC_DEV=$(ls /sys/class/udc | head -1)
echo "$UDC_DEV" > UDC

echo "UAC2 Gadget configured on $UDC_DEV"
echo "ALSA device: UAC2 PCM (check with 'aplay -l')"
