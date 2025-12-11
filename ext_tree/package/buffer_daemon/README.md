# Buffer Daemon for UAC2 Feedback Control

Real-time buffer monitoring daemon for UAC2 audio feedback control.

## Features

- Real-time buffer monitoring with 15-second averaging
- Hysteresis-based feedback control to prevent oscillation
- Gradual adjustment with configurable step size
- Filtering of invalid/empty buffer readings
- Sysfs feedback control for UAC2 gadget
- Low CPU overhead for audio-critical systems

## Usage

The daemon monitors `/proc/asound/card0/pcm0p/sub0/status` and adjusts
`/sys/devices/virtual/u_audio/uac_card1/feedback` to maintain optimal
buffer fill ratio and prevent audio delays.

## Buffer Zones

- **Very High** (>65%): Slowdown to 999000
- **High** (55-65%): Slight slowdown to 999500  
- **Moderate** (35-55%): Maintain nominal 1000000
- **Low** (20-30%): Slight speedup to 1001000
- **Very Low** (<20%): Speedup to 1002000

## Installation

Built as part of PureCore Buildroot system. Enable with:

```
make menuconfig
  -> Target packages
    -> Audio and video applications
      -> [*] buffer_daemon
```

## License

MIT License - see LICENSE file for details.