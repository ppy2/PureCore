#!/bin/sh
# Script to set MCLK = 512x in PLL mode

# Set MCLK multiplier via sysfs
echo 512 > /sys/devices/platform/ffae0000.i2s/mclk_multiplier

# Update configuration file
sed -i 's/^MCLK=.*/MCLK=512/' /etc/i2s.conf

echo "MCLK multiplier set to 512x for PLL mode"
sync