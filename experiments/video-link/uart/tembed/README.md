# T-Embed display for the C5VRX video link

Firmware for the LilyGo T-Embed CC1101 (ESP32-S3, 320x170 ST7789) that shows
the frames a XIAO ESP32-C5 running the C5VRX video link grabs from an analog FPV
transmitter, and sends back channel, scan and picture commands. The C5 does
all the radio and video work: it FM-demodulates the 5.8 GHz signal in
software, locks onto the PAL/NTSC sync and streams 224x168 grey frames over
UART at 4 Mbaud. This board decodes, draws and shows the link status in the
left margin (VIDEO / NO SIG / NO LINK / SCAN, channel, frequency, standard,
frames per second, picture mode and delta, RF gain, grab error, link
errors per second).

The protocol lives in `../protocol/link_proto.h`, shared with the C5 firmware
(the build adds `../protocol` to the include path); see
[../README.md](../README.md) for the design, the frame rates and the bench
checklist.

## Wiring

| XIAO ESP32-C5 | T-Embed UART connector |
|---|---|
| D6 (GPIO11, TX) | RXD (GPIO44) |
| D7 (GPIO12, RX) | TXD (GPIO43), commands |
| GND | GND |

Keep the wires short (under 20 cm) at 4 Mbaud. The simplest setup powers
both boards over their own USB ports and leaves 3.3 V unconnected between
them; "Power from the T-Embed" below runs the XIAO off the T-Embed instead.

## Controls

| Input | Action |
|---|---|
| Turn the encoder | Previous / next channel (bands R, A, B, E, F, L; untunable ones skipped) |
| Press the encoder | Rotate the picture by 180 degrees |
| Hold the encoder 0.7 s | Scan: "SCAN MODE" on screen, stops on the first channel with live video, beeps, shows "LOCK R6 5843". Hold again to stop scanning and stay on the current channel. |
| Press BACK | Picture mode: AUTO (fastest, near-lossless), FINE (+/-1 level), RAW (8-bit, about 10 fps) |
| Hold BACK 0.7 s | Horizontal smoothing on / off ("S0" in the status column when off) |

The beep: the speaker amplifier's LRCLK and the display's reset share
GPIO40 on this board, so the display is reset by the beep and started again
afterwards (it blanks for about a quarter of a second). `BEEP 0` in
`src/main.cpp` turns it off. If turning the encoder steps backwards, or two
channels per click, change `ENC_DIR` / `ENC_COUNTS_PER_DETENT`.

The UART follows the C5's baud rate: with bytes arriving but no valid packet
for 1.5 s it switches between 4 and 5 Mbaud.

Auto levels: every frame's histogram sets the black point (darkest 1 %) and
the white point (brightest 0.5 %, usually the OSD), followed slowly and
stretching at most 1.6x, because real cameras and transmitters do not reach
the standard white the receiver assumes. `AUTO_LEVELS 0` in `src/main.cpp`
shows the receiver's levels as they are.

`SERIAL_STATUS 1` in `src/main.cpp` prints the link statistics (frames per
second, KB/s, CRC errors, the receiver's state) on the USB serial port once a
second.

The UART plug on the T-Embed works loose easily: if both directions go
silent at once (T-Embed "NO LINK", C5 `cmds rx` stuck), re-seat it.

## Build and flash

```bash
pio run -d tembed                    # official espressif32 6.12.0 + Arduino-ESP32 2.0.17
pio run -d tembed -t upload          # native USB
```

`platformio.ini` points `packages_dir` at `~/.platformio/packages-s3`: the
official platform and the pioarduino platform of the C5 project use the same
package names in different versions, and sharing one package store made each
build replace the other's esptool and RISC-V toolchain. On the development
host the large packages in that store are directory junctions into
`~/.platformio/packages`:

```bat
mklink /J %USERPROFILE%\.platformio\packages-s3\framework-arduinoespressif32 %USERPROFILE%\.platformio\packages\framework-arduinoespressif32
mklink /J %USERPROFILE%\.platformio\packages-s3\toolchain-xtensa-esp32s3 %USERPROFILE%\.platformio\packages\toolchain-xtensa-esp32s3
```

Without them PlatformIO simply downloads the packages into the separate store.

## C5 side

Build the parent directory (`experiments/video-link/uart`) with the
`xiao_c5_link` environment (final: no console output while running) or
`xiao_c5_link_debug` (desk tools and a statistics line every second). It boots on
Raceband R6 (5843 MHz, `CONFIG_C5VRX_LINK_CHANNEL`) with the wide analog
filter, in picture mode AUTO (`CONFIG_C5VRX_LINK_PICTURE_MODE`), and starts
streaming at once (`CONFIG_C5VRX_LINK_STREAM`). Console keys on its USB
port: `S` streaming on/off, `P` picture mode, `N` scan, `M` smoothing; the
debug build adds `g` (one frame for `tools/link_frame.py`) and `v` (raw I/Q
for `tools/link_dump.py`) and prints a `[STREAM]` statistics line every
second.

## Power from the T-Embed

The XIAO can run from the T-Embed instead of its own USB, so one cable powers
both boards. The proven wiring is the T-Embed's 3.3 V pin on the UART
connector (next to GPIO43/44; it comes from the always-on ME6217 LDO, so it
is live whenever the T-Embed is) to the **BAT+ pad** on the underside of the
XIAO, plus the GND already wired. Feeding the same 3.3 V into the XIAO's 3V3
pin does not work: that pin is the output of the XIAO's own regulator, and
the board does not boot from it. BAT+ goes into the regulator's input, which
does start at 3.3 V even though it is meant for a 3.7 V cell.

The C5 draws roughly 100 mA in link mode (receiver always on, CPU at
240 MHz), with short peaks while its radio calibrates at boot. If the XIAO
restarts in a loop, the T-Embed's rail cannot supply it; power the XIAO over
USB instead.

The T-Embed has no 5 V output: USB 5 V is consumed by the BQ25896 charger,
and the battery connector is a single cell (3.0-4.2 V), not 5 V.

**Warning:** BAT+ is a battery terminal, so with this wire soldered the XIAO
charges it whenever the XIAO's own USB is plugged in, pushing current back
into the T-Embed's 3.3 V rail. The ESP32-S3 is rated for 3.6 V maximum.
Before flashing the C5 over USB, unsolder or unplug that wire, or at the very
least keep the T-Embed powered so its rail is held at 3.3 V. Note also that
with the XIAO powered from the T-Embed, unplugging its USB no longer
power-cycles it, so entering the ROM download mode needs the RESET button as
well as BOOT (see `tools/flash_pio.py`).
