#!/bin/sh

# USB UAC2 to I2S Audio Router
# Routes USB audio input to I2S DAC output with zero-copy passthrough

# Kill existing router instances
killall uac2_router 2>/dev/null

# Start custom router daemon
# Default: 48kHz (will auto-detect from USB stream)
/root/uac2_router 48000 &

echo "USB UAC2 -> I2S routing started (PID: $!)"
echo "Supports: 44.1, 48, 88.2, 96, 176.4, 192, 352.8, 384 kHz"
echo "Stop with: killall uac2_router"
