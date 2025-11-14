#!/bin/sh

# XingCore-compatible UAC2 Audio Gadget
# Based on sniffed USB descriptors from 152a:8852

CONFIGFS="/sys/kernel/config"
GADGET="$CONFIGFS/usb_gadget/g1"

modprobe libcomposite

mkdir -p "$GADGET"
cd "$GADGET"

# Device Descriptor (XingCore clone)
echo 0x152a > idVendor
echo 0x8852 > idProduct
echo 0x0312 > bcdDevice
echo 0x0200 > bcdUSB
echo 0xEF > bDeviceClass
echo 0x02 > bDeviceSubClass
echo 0x01 > bDeviceProtocol
echo 64 > bMaxPacketSize0

# Device Strings
mkdir -p strings/0x409
echo "XingCore" > strings/0x409/manufacturer
echo "XingCore USB Hi-Resolution Audio" > strings/0x409/product
echo "" > strings/0x409/serialnumber

# UAC2 Function
mkdir -p functions/uac2.0

# Playback ONLY (XingCore has no capture)
echo 3 > functions/uac2.0/p_chmask
echo "44100,48000,88200,96000,176400,192000" > functions/uac2.0/p_srate
echo 4 > functions/uac2.0/p_ssize
echo 1 > functions/uac2.0/p_mute_present
echo 1 > functions/uac2.0/p_volume_present

# No capture
echo 0 > functions/uac2.0/c_chmask

echo "XingCore USB Audio" > functions/uac2.0/function_name

# Configuration (self-powered, 100mA like XingCore)
mkdir -p configs/c.1
mkdir -p configs/c.1/strings/0x409
echo "Audio" > configs/c.1/strings/0x409/configuration
echo 100 > configs/c.1/MaxPower
echo 0xC0 > configs/c.1/bmAttributes

ln -s functions/uac2.0 configs/c.1/

# Bind to UDC
UDC_DEV=$(ls /sys/class/udc | head -1)
echo "$UDC_DEV" > UDC

echo "XingCore clone UAC2 configured on $UDC_DEV"
