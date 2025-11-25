#!/bin/sh
# Script to set MCLK = 1024x in PLL mode

# Set MCLK multiplier via sysfs
echo 1024 > /sys/devices/platform/ffae0000.i2s/mclk_multiplier

# Update configuration file
sed -i 's/^MCLK=.*/MCLK=1024/' /etc/i2s.conf

echo "MCLK multiplier set to 1024x for PLL mode"
sync