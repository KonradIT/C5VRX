# 802.11p receive setup (`CONFIG_C5VRX_PHY_11P`)

`main/rf.c` calls the ESP32-C5 PHY library's `phy_11p_set(1, 0)`, its setup
for 802.11p (the 5.9 GHz vehicular Wi-Fi band), after RF start and after
every channel or frequency-offset change. The Kconfig option
`CONFIG_C5VRX_PHY_11P` ("C5VRX RF front end") is on by default and applies to
every build. The boot log's `RF ready` line ends in `11p=on` or `11p=off`.

It was reported to improve reception from 5.75 to 5.99 GHz (observation
credited to SushiDude, @Ready4Sushi on X). It has not been measured here
yet: that range is the report, not a known limit, and the effect below
5.75 GHz is unknown.

## What it writes

From the disassembly of the ESP-IDF 6.0.1 `libphy.a` (the PlatformIO
build's); the same as the ESP-IDF 6.0.2 table on upstream branch
`codex/analog-lock-phy-lab` (issue #155), whose lab offers a reversible A/B:

| State | Mask / registers | Value |
|---|---|---|
| `phy_param` | offsets `0x26`, `0x27` (enable, mode) | `1`, `0` |
| `0x600A7CE4` | `0x0000001c` | `0x00000010` |
| `0x600A7030` | bit 5 | clear |
| `0x600A7048` | `0x00007f00` | `0x00004000` |
| `0x600A71C4` | `0x00fe0000` | `0x00440000` |
| analog I2C block `0x67`, host `1` | registers 6-13 | all `60` |

`phy_chip_set_chan()` replays it from the two `phy_param` flags after every
channel set. `phy_chip_set_chan_offset()` does not, and redoes the band setup
(`phy_set_channel_rfpll_freq_new()` calls `phy_band_change()`), so `rf.c`
applies it again, last, after every tune. There is no run-time switch:
`phy_11p_set(0, 0)` writes the library's defaults, not necessarily the state
from before.

## Comparing

Build once with the option off (menuconfig, "C5VRX RF front end", or
`CONFIG_C5VRX_PHY_11P=n` in an sdkconfig overlay) and once on. At the same
position compare the picture, the gain the receiver settles on and, in the
video-link debug build, the `[STREAM]` figures, on channels both above and
below 5.75 GHz.
