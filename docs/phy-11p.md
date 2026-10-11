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

## What the filter writes mean (C5VRX-4 findings)

`experiments/c5vrx-4` later identified analog I2C block `0x67`, host `1`,
registers 6-13 as the analog receive filter (its `phy_rx_lab_filter_*`, after
the ESPARGOS esp-sdr model): registers 6/7 hold the RX0 code, 8-13 a second
stage, and 60 is the narrowest code it uses (`predemod_filter_code()`
saturates there, "the code phy_11p_set() writes"). So `phy_11p_set(1, 0)`
puts the whole filter at that narrowest setting. That this is most of its
effect is an inference, not a measurement.

C5VRX-4's board measurements (2026-10-06, VTX off, noise at the MODEM_DIAG
tap; commits `a22bf67`, `53a1e6b`):

- The width was 19.4 MHz at every RX0 code and with the digital BW20 and BW40
  filter alike: the tap sits ahead of the digital filter.
- The second stage narrows it: +16 codes gave 15.6 MHz and noise bandwidth
  22.2 -> 17.5 MHz (1.0 dB); +24 codes gave 14.4 MHz and 20.7 -> 15.4 MHz
  (1.3 dB). Its range-edge profile refuses anything narrower than 14 MHz.
- Whether that lower noise bandwidth extends range is not measured there
  either. The width of the all-60 state that 11p writes is not measured.

Do not combine 11p with C5VRX-4's fixed or range-edge bandwidth: both write
registers 6-13, and C5VRX-4 takes their boot values as the chip's
calibration. C5VRX-4's native-AGC restart patch writes `0x600A71C4` bits
25:23, which overlaps bit 23 of the 11p field; the root receiver has no such
patch and C5VRX-4 refuses its 11p lab in native AGC mode, so no build
combines them today.

The root receiver tunes up to 5945 MHz (R8 and E6-E8 from the 5885 MHz
centre, as C5VRX-4 does), which covers the band of the 11p report. The RX DC
calibration's highest point is 5855 MHz.

## Comparing

Build once with the option off (menuconfig, "C5VRX RF front end", or
`CONFIG_C5VRX_PHY_11P=n` in an sdkconfig overlay) and once on. At the same
position compare the picture, the gain the receiver settles on and, in the
video-link debug build, the `[STREAM]` figures, on channels both above and
below 5.75 GHz. For a noise-bandwidth figure instead of a picture, the C5VRX-4
`$` filter sweep (pinned PHY library only) includes the 11p-equivalent code
60; it has no recorded result yet.
