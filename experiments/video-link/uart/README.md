# UART video link to a LilyGo T-Embed

This experiment turns the XIAO ESP32-C5 into a receiver that has no analog
output: the C5VRX receiver (the repository's `main/`) plus
`components/video_link_uart`, which demodulates the 5.8 GHz FM video in
software, grabs 224x168 grey frames and streams them over UART to a LilyGo
T-Embed CC1101. The T-Embed shows them on its 320x170 display and sends back
commands from its encoder and buttons (`tembed/`).

```text
 MODEM_DIAG Q4/I4 40 MS/s -> PARLIO RX -> 32 KiB GDMA ring
        |
   grab.c: FM demod (angle table, phase steps), sync lock, flywheels,
           224 x 168 rows, two fields per frame
        |  every row, as soon as it is captured
   link_tx.c: row queue -> NL row codec -> CRC-16 packets -> staging
        |  encoded while the grabber waits for lines
   uart_link.c: UART1 4 Mbaud (GPIO11 TX, GPIO12 RX)
        |
   T-Embed: parser + decoder (same link_proto.h), RGB565, ST7789;
            encoder / BACK button -> commands
```

## Layout

| Path | Contents |
|---|---|
| `CMakeLists.txt`, `sdkconfig.defaults`, `sdkconfig.debug.defaults`, `platformio.ini` | The ESP-IDF project: the receiver (`../../../main`) plus the component below |
| `components/video_link_uart/` | C5 sender: `grab.c` frame grabber, `link_tx.c` row codec and sender, `uart_link.c` UART, `link.c` stream task, scan and console keys; `Kconfig` with the link options; `fm_loop.bsasm`, the loopback demodulator for the `v` dump |
| `protocol/link_proto.h` | The protocol, shared by the C5 sender, the T-Embed and the host tests |
| `tembed/` | The T-Embed receiver, its own PlatformIO project ([tembed/README.md](tembed/README.md)) |
| `test/` | Host simulator and tests |
| `tools/` | `link_dump.py`, `link_frame.py` for the debug build's dumps |

## Build and flash

PlatformIO, in this directory (from PowerShell or cmd on Windows; the
ESP-IDF tool installer refuses to run under Git Bash):

```powershell
pio run -e xiao_c5_link            # final build: nothing on the console
pio run -e xiao_c5_link_debug      # desk build: statistics and dumps
python ../../../tools/flash_pio.py xiao_c5_link
```

`tools/flash_pio.py` finds the build here and writes it through the ROM
download mode (unplug, hold BOOT, plug in, release BOOT when it asks).

ESP-IDF v6.0.x, in this directory, as for `experiments/c5vrx-4` (not run on
this setup):

```sh
idf.py -DIDF_TARGET=esp32c5 build
idf.py -DIDF_TARGET=esp32c5 -B build-debug -DSDKCONFIG=sdkconfig.debug -DVIDEO_LINK_DEBUG=1 build
```

## How it attaches to the receiver

This is a separate ESP-IDF project that reuses the receiver's `main/`
component, as `experiments/c5vrx-4` does. `main/` carries only hooks:

- `main/video.h` / `video.c`: read access to the raw RX ring
  (`video_rx_ring()`, `video_rx_write_offset()`, `video_rx_max_descriptor()`,
  `video_copy_recent_rx()`) and to the gain (`video_set_rx_gain()` puts the
  receiver's gain control in MANUAL). Always compiled; the normal build does
  not use them.
- Behind `C5VRX_VIDEO_LINK_EXPERIMENT`, which this project's `CMakeLists.txt`
  defines: no PARLIO TX, no flight BitScrambler program and no menu (the
  ~98 KiB menu raster is a stub and `video_set_menu_mode()` returns at once),
  so the BitScrambler is free for the `v` loopback dump. Once the RX ring
  runs, `video_start()` sets the wide (BW40) filter and calls
  `video_link_start()`. The console task offers every key to
  `video_link_console()` before the receiver's lab keys, so the link's `S`,
  `P`, `N`, `M`, `v`, `g` shadow lab keys of the same letters. The reset to
  the survival gain after every retune is skipped: the link owns the gain.
- `sdkconfig.defaults` here turns Direct Gain V3 off: in MANUAL it never
  acts, and its 200 us observer timer would only preempt the frame grabber.
  Phase8 HR, a DAC-path demodulator, goes with it (it only compiles with V3;
  `experiments/c5vrx-4` turns both off too).

`components/video_link_uart` implements `video_link_start()` and
`video_link_console()`, declared in `main/video.h`. It is linked as a whole
archive, so `main/` needs no dependency on it. The receiver's RX ring is
`RAW_RING_BYTES` (32 KiB = 819.2 us); `VIDEO_RX_RING_BYTES` in `video.h` must
match (static assert).

The receiver on this branch also applies the vendor 802.11p receive setup,
see [docs/phy-11p.md](../../../docs/phy-11p.md); compare the link with it off
and on.

## Protocol (version 2)

Defined once in `protocol/link_proto.h`, compiled into the C5 firmware, the
T-Embed firmware and the host tests.

- Packets: `A5 5A | type | a | b | len | payload | crc16`. CRC-16/CCITT
  over type..payload. The parser re-examines the bytes of a rejected packet,
  so a packet hidden inside a corrupted one is still found.
- ROW packets carry one row, coded **NL**: near-lossless DPCM (every pixel
  within +/-delta of the grabbed value, delta 0 is lossless) with a Rice code
  whose parameter comes from the previous row. A row the code would make
  longer than raw goes raw.
- The INFO packet closes a frame (standard, channel, gain, errors, delta,
  scan state, scan lock counter, timing) and is sent alone while scanning or
  without picture.
- Commands from the display: scan start/stop, channel step, set channel,
  picture mode, smoothing.

## Picture modes and frame rate

| Mode | Rows | Bytes per frame | Frame rate (PAL, 4 Mbaud) |
|---|---|---|---|
| AUTO (default) | NL, delta chosen per frame to fit the link | about 14-15 KB | about 24.5 fps (grabber-limited, 25 max) |
| FINE | NL, delta 1 | about 20 KB | about 21 fps |
| RAW | 8-bit | 39.2 KB | about 10-11 fps (link-limited) |

Figures are from the host simulator (see below). AUTO sizes the next frame
for the CPU time a frame takes (grab plus the encoding left after it); it
settles at delta 2-3 on the test pattern. On the real frames captured on the
bench (horizontally smoothed), delta 2 costs 90 bytes per row at 45 dB PSNR
and delta 3 costs 78 bytes at 42 dB. The previous 8-bit format needed 225
bytes per row, and its 4-bit option (truncated, not dithered) gave 33 dB.

At 5 Mbaud (`CONFIG_C5VRX_LINK_UART_BAUD`; both UARTs support it, untested
on the bench) AUTO stays at the grabber's 25 fps and spends the extra room on
a lower delta. The T-Embed follows either rate by itself.

## Review of the first implementation (commit 8b7005c)

Hardware at 01:16 on 2026-09-19 showed 8-11 complete frames per second,
1-3 timeouts per second, 31-88 rows per second found only by the whole-line
search, sync jitter around 300 ns, and a picture that crept upwards.

| Problem | Cause | Fix |
|---|---|---|
| Picture creeps up the screen | Every row re-anchored the grid on its own noisy edge, and the line period was nudged by 0.3 x error / 3 lines on every row pair: a random walk. Once a prediction was more than half a line off, the whole-line search locked onto the neighbouring line and the numbering slipped. Nothing re-checked the vertical position. | Flywheel: grid steered a quarter of the way to each edge; line period from the steered grid over 400+ lines; the vertical interval checked every field. |
| Jitter about 300 ns, sideways wobble | Pixels placed at each raw edge | Pixels placed on the steered grid (sim: 10-30 ns). |
| Frame timeouts | Rows waited at most 30 ms for their line, so the field loop ran ahead of real time and a late row starved for the whole grab | Rows wait until the grab deadline. |
| "lost" errors at low signal | FM clicks (phase slips, about 0.1 us dips) passed as sync edges | 16-sample running sum and a check that sync level follows for 0.4-2.8 us. |
| Lines broken mid-frame | The receiver's AGC changed gain during a grab | AGC manual in link mode; gain moved between frames from power and clipping. |
| About 10 fps ceiling | 8-bit rows (38.7 KB per frame) at 4 Mbaud; each grab waited for the next field start, and the frame was sent only after the grab | NL rows, grab starts in the field on air (two fields per frame), rows encoded and sent while the grab waits for lines. |
| A single late row cost 40 ms | A row whose line had left the ring waited two fields for the next chance; the write position was known only per 102 us DMA descriptor | Write position from the timebase (about 1 us, checked at every descriptor switch), the next line of the field used instead of waiting, a few stubborn rows given up (the display keeps the old row). |
| Weak framing | CRC-8 over 230-byte packets, fixed sizes, no recovery inside a bad packet | CRC-16, length byte, re-scanning parser (host test: all 1952 intact packets recovered from a damaged stream, none invented). |
| Simulator could not be built from a clean checkout | `.gitignore` rule `sdkconfig.*` also matched the stub `test/stubs/sdkconfig.h` | Exception added. |

Smaller items: two 37.6 KB frame buffers on the C5 (one now); the T-Embed
drew a frame only when row 167 arrived (the INFO now closes it); per-row
floating-point work reduced (the C5 has no FPU).

## Channel scan

Hold the encoder button for 0.7 s: the C5 steps through every channel it can
tune (bands R, A, B, E, F, L; R8, E6, E7, E8 are outside the C5's 5 GHz
window), probes each for up to 40 ms for horizontal sync and stays on the
first whose carrier is within 10 MHz of the channel. The T-Embed shows
"SCAN MODE" and the channel being probed, then beeps and shows "LOCK ..." on
lock. Hold again to stop on the current channel; turning the encoder also
stops the scan and steps the channel.

The probe was checked against the raw captures from the bench: 10 of 12
captures show video within 1.2-3.9 ms, carrier offsets -2.3 to +0.7 MHz; of
the other two, one had no carrier. In the simulator acquisition works up to
about +/-8 MHz carrier offset.

## Bench results (2026-09-19)

PAL camera on R6 5843 MHz, 4 Mbaud, picture mode AUTO, first build of this
design on hardware:

| | Simulator | Hardware |
|---|---|---|
| Frames per second (C5 grabs / T-Embed draws) | 24.6 | 24-25 / 23.5-24 |
| Grab time | 39-40 ms | 39-40 ms, all frames complete |
| CPU per row: capture / encode | 90 / 55 us | 97-99 / 39-41 us |
| Delta, bytes per frame | 3, 14.5 KB | 3, 12.7-14.5 KB |
| Sync jitter | 10-30 ns | 21-68 ns (the first build had about 300 ns) |
| Link errors | none | 0-14 CRC errors over thousands of rows, no bad rows |

The channel scan found the drone from a few channels away, locked on F6
5840, beeped and showed the lock; the encoder steps channels and BACK
switches picture modes.

Found and fixed on the bench:

- The T-Embed's baud-rate follower compared times as unsigned numbers; a
  frame drawn inside the packet parser made "last good packet" newer than
  the loop's "now", the difference read as 49 days, and the display switched
  to 5 Mbaud every few seconds (picture freezing, "NO LINK"). Signed ages
  now.
- The stream task never blocked any more (grab at priority 10, sender
  non-blocking), so the console task (priority 1) starved and console keys
  were ignored. The stream task now yields one tick per frame.
- The picture looked dimmer than with the first build: the OSD no longer
  reached white. The level code is unchanged and exact in the simulator; on
  the bench the measured blanking level sat about 1.2 phase units higher.
  The T-Embed now applies auto levels (black at the darkest 1 %, white at
  the brightest 0.5 %, followed slowly, at most 1.6x), which also covers
  cameras whose white is below standard.
- The T-Embed's UART plug had worked loose: both directions went silent at
  once.

Open items:

- The timebase check disagrees with the DMA descriptor switches by up to
  +/-60 us on hardware, so every grab uses the descriptor-granular ring
  bounds (`timebase bad` in `[STREAM]`). No rows were late anyway. Likely
  cause: interrupts between the descriptor-register read and the timer read;
  a check with timer reads on both sides of the register read would tell.
- About 460 rows per second (12 %) are found only by the whole-line search,
  at errors of 0-1 samples, so the narrow search misses edges that are where
  predicted. One suspect: the descriptor register running ahead of the data,
  so the narrow search reads the edge before it is in memory and the slower
  wide search, a few hundred microseconds later, finds it.

## Host simulator and tests

`test/run_tests.sh [CAPTURES]` (bash, from this directory; Git Bash works on
Windows) builds and runs everything below in about two minutes; with a
directory of bench captures it also replays those.

- `link_test.c`: CRC, codec round trips at every delta (decoder output equals
  the encoder's reconstruction, error within delta), parser under bit flips,
  drops and inserted garbage, compression statistics on real frames.
- `grab_sim.c`: synthetic PAL/NTSC FM video (noise, carrier offset, camera
  clock offset, gaps) or bench captures, through the real `grab.c` and
  `link_tx.c`, a model of the UART, and the T-Embed's parser and decoder;
  every decoded row is checked against the grabbed one.
- The simulator's clock follows a model of the C5's CPU: the hot paths report
  their RISC-V instruction counts (`SIM_COST`, from the -O2 disassembly), at
  1.73 cycles per instruction, calibrated so that an acquisition window takes
  257 us (255 us measured on hardware). `--cpu host` uses host timing instead.

## Checking it on the bench

The final build (`xiao_c5_link`) prints nothing while it runs. The desk
build, PlatformIO environment `xiao_c5_link_debug`
(`CONFIG_C5VRX_LINK_DEBUG`), adds the `[STREAM]` line every second, `[LOST]`
traces and the `v` / `g` dumps used by `tools/link_dump.py` and
`tools/link_frame.py`. On the T-Embed,
`SERIAL_STATUS 1` does the same for its side.

What to look at in `[STREAM]`:

- `frames`, `complete`, and `grab avg`: about 25 frames of 40 ms on PAL.
- `cpu=` and `encode=` (us per row): 97-99 and 39-41 on the bench (the
  simulator predicts 90 and 55). Much higher values mean rows will run late.
- `late`, `sub` (rows taken from the next line), `timebase bad` and
  `lead=` (timebase against the DMA, expected within a few microseconds;
  `bad` grabs fall back to the slower descriptor bounds).
- `delta` and `bytes`: AUTO's choice; `flush` above zero means the UART is
  the bottleneck.
- `cmds rx` and `err`: commands from the T-Embed.

The T-Embed prints fps, link KB/s, CRC errors and the receiver's state on its
USB serial port once per second.

## Known limitations

- GPIO40 is both the display reset and the speaker amplifier's LRCLK on the
  T-Embed CC1101: the lock beep resets the display, which is started again
  after the beep (the screen blanks for about a quarter of a second).
  `BEEP 0` in `tembed/src/main.cpp` turns the beep off.
- The encoder's counts per detent and direction are set from LilyGo's
  examples (`ENC_COUNTS_PER_DETENT`, `ENC_DIR`), not measured.
- The scan locks onto the first channel with video in band order; a
  transmitter between two channels shows as the first one within 10 MHz.
- The CPU model is an estimate until the `[STREAM]` figures confirm it.
