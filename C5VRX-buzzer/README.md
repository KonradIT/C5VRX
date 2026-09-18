# C5VRX-buzzer: FPV transmitter proximity buzzer

Standalone firmware for the Seeed Studio XIAO ESP32-C5 that sweeps the
5 GHz band and drives a buzzer: the closer an analog video transmitter, the
higher and louder the tone, whichever channel it uses. The receiver steps
through the standard 5 GHz Wi-Fi channels (36..64, 100..144, 149..177, i.e.
5180..5885 MHz in 20 MHz steps, `CONFIG_C5VRX_BUZZER_SWEEP_*`), measures the
carrier power on each and follows the strongest one; the ~20 MHz analog
passband covers the gaps between channel centres. With
`CONFIG_C5VRX_BUZZER_SWEEP=n` it listens on one Wi-Fi channel instead
(`CONFIG_C5VRX_BUZZER_WIFI_CHANNEL`, centre = 5000 + 5 x channel MHz: 169 =
5845 MHz for Raceband R6, 173 = 5865 MHz for band A1). There is no CVBS
output, no DAC, no BitScrambler in this build. The RF front-end and the raw
I/Q capture are the proven C5VRX-3 path; only the consumer of the samples
changed, plus a channel switch between visits.

## Why carrier power, not the demodulated video

FM demodulation removes amplitude information on purpose: the recovered CVBS
waveform has the same swing whether the transmitter is 1 m or 100 m away,
until it dissolves into static. Feeding the demodulated signal to a buzzer
would therefore not track distance at all. What tracks distance is the
received carrier power, i.e. the magnitude of the I/Q samples before
demodulation. Because C5VRX freezes the hardware AGC and forces a fixed
receiver gain index, that magnitude is proportional to the RF input for a
given gain index.

The captured samples are only 4 bits per axis (`Q[9:6]`, `I[9:6]`), about
24 dB of range, so `meter.c` runs a small software AGC: it keeps the mean
`I^2 + Q^2` of the strongest channel inside a window by stepping the forced
gain index (one index for the whole sweep). The level of a channel is its
carrier power in dB above the idle noise floor at maximum gain:

```text
level_db = (GAIN_MAX - gain_index) * DB_PER_INDEX + 10 log10(power / NOISE_POWER)
strength = max over channels (level_db) / RANGE_DB
```

`NOISE_POWER` covers the idle reading at maximum gain seen on hardware (0.05
on one boot, a gain-independent offset of about 1.0 on others; 2.0 used);
below a window power of 3.0 a channel holds nothing above the noise at the
current gain and reads 0 dB. `DB_PER_INDEX` is the gain change of one PHY
gain index, measured on hardware (see Status): 1.0 dB per index makes levels
taken at different gains agree within about 1 dB, so the per-channel history
survives gain steps. `RANGE_DB` (45) is the level mapped to the highest
pitch: a 25 mW transmitter reads ~14 dB at 1 m and ~30 dB when touching the
board.

The band is shared with 5 GHz Wi-Fi, whose packets raise single windows by
10 dB or more. Every channel visit therefore takes the minimum power of the
eight 102 us windows that make up the 819 us ring (a packet shorter than the
ring leaves at least one quiet window), and the reported level is the
minimum over the last two visits: a continuous FM carrier keeps those minima
up, bursts do not. A saturated Wi-Fi channel right next to the board can
still read as a carrier; that is real RF power and cannot be told apart
without demodulation.

Every channel starts with a 1 dB idle floor; the `z` key measures the real
floor per channel over ten sweeps (transmitter off) when an environment
needs it. The strongest channel's level above its floor is mapped to 0..1,
smoothed and mapped to a tone of 300..3500 Hz (log scale) with a PWM duty of
3..50 %.

## Sweep timing

One visit is the driver's `esp_wifi_set_channel()` (the retune time is
printed at boot and in every telemetry line), then at least 1 ms for the PLL
to settle and the ring to be rewritten, then about 1 ms of arithmetic over
the 32 KiB ring. After each retune the firmware re-applies what the driver's
channel switch may disturb: frozen AGC, BW20 analog filter, forced gain
index, disabled MAC TX queues, and it re-arms the modem dump engine if bit 31
of `DUMP_CTRL` dropped (`rearm` in the telemetry counts that). The gain
decision is taken once per sweep from the strongest channel, so the AGC
converges in a few sweeps. `CONFIG_C5VRX_BUZZER_SWEEP_MIN_MHZ=5645` skips the
Wi-Fi-only lower half of the band (FPV bands live in 5645..5945 MHz) and
roughly doubles the sweep rate; `CONFIG_C5VRX_BUZZER_SWEEP_STEP_MHZ=40`
visits every other channel.

## Jammer discrimination

Carrier power alone cannot tell an FPV transmitter from anything else that
radiates inside the passband: a noise jammer, an unmodulated carrier or a
Wi-Fi access point streaming without a pause all read as "signal". Two tests
run on every channel strong enough to judge (minimum window power at least
6, about 6 dB above the noise at maximum gain; weaker channels are passed
untested, so the detection limit is unchanged):

1. **Envelope** (`CONFIG_C5VRX_BUZZER_REJECT_NOISE`). Frequency modulation
   keeps the envelope constant, so the per-sample power `I^2 + Q^2` of an
   FM carrier barely varies: variance / mean^2 is a few percent
   (quantisation) and about 0.4 at 6 dB SNR. Gaussian noise has an
   exponential power distribution, variance / mean^2 = 1 (about 0.8 with the
   mild clipping the AGC allows). Threshold 0.7, minimum over the eight
   windows so a Wi-Fi packet in the ring cannot fail a real carrier.
2. **Line sync** (`CONFIG_C5VRX_BUZZER_REQUIRE_SYNC`). Six ring blocks
   (614 us) are snapshotted in time order, FM-demodulated with the cross
   product of consecutive samples (`I[n-1] Q[n] - Q[n-1] I[n]`, proportional
   to the sine of the phase step, which stays monotonic for FPV deviations at
   40 MS/s), summed 32 samples per output (1.25 MS/s) and autocorrelated at
   one television line: 80 samples for PAL (64.0 us), 79 for NTSC (63.6 us).
   Every line of composite video starts with the same sync pulse and
   blanking, so real video correlates above 0.5; noise, an unmodulated
   carrier, a noise-modulated carrier or a swept jammer stay near 0.
   Threshold 0.25. This also rejects a transmitter with no camera attached
   (an unmodulated carrier); disable the option if that matters more than
   CW jammers.

A channel drives the buzzer only if it passed in this or the previous visit,
so one visit during a vertical blanking interval or a burst of noise does
not drop the tone. Rejected channels still count for the software AGC (a
strong jammer lowers the gain and masks weaker transmitters, which is the
physics of a single front-end) and are listed with a `J` in the spectrum
line and in a `[jammer]` telemetry line. The tests cost about 2 ms per
tested channel per sweep, i.e. nothing while the band is quiet.

## Wiring

| Signal | XIAO pad | GPIO | Notes |
|---|---|---|---|
| Buzzer | D2 | 25 | default `CONFIG_C5VRX_BUZZER_GPIO=25`; passive piezo between the pad and GND (a series resistor of 100 Ohm is kind to the pad) |
| MODEM_DIAG lanes | D0, D1, D3, D4, D5, D10 and back pads MTDO, MTCK | 1, 0, 7, 23, 24, 10, 5, 4 | driven at 40 MHz while receiving, leave unconnected |

Seeed's pinout and the Arduino variant both map pad **D2 to GPIO25**; GPIO3
is the A2/MTDI pad on the back of the module. If the buzzer really sits on
GPIO3, set `CONFIG_C5VRX_BUZZER_GPIO=3` in `sdkconfig.defaults` or with
`pio run -t menuconfig`. Both pads are free in this firmware: the two I/Q
lanes that C5VRX-3 places there (GPIO25, GPIO3) moved to the former DAC pads
D4/D5. Any other free GPIO works as well, except the lane GPIOs above.

The XIAO ESP32-C5 has no on-board antenna: the u.FL connector needs an
external 5 GHz antenna, otherwise the receiver only hears transmitters within
a few metres.

## Build and flash

```bash
pip install pioarduino          # PlatformIO core fork required by the pinned platform
pio run                         # environment xiao_c5, ESP-IDF v6.0.x
pio run -t upload               # bootloader + partition table + application over USB
pio device monitor              # 115200 baud telemetry and keys
```

The pinned pioarduino platform and the toolchain script
(`../tools/pio_idf_toolchain.py`, which installs the exact RISC-V toolchain
ESP-IDF 6.0 requires) are explained at the top of `platformio.ini`; the
conservative flash/CPU settings (8 MB DIO at 40 MHz, 160 MHz CPU, PVT off)
follow the receiver's validated ESP32-C5 v1.0 configuration. Run `pio` from
PowerShell, cmd or a POSIX shell: ESP-IDF's tool installer refuses MSYS/Git
Bash. Outputs land in
`.pio/build/xiao_c5/` including the merged `firmware.factory.bin` for offset
`0x0`.

## Serial telemetry and keys

At boot the buzzer plays a two-tone chirp before the RF bring-up (wiring
check); three low beeps mean the RF front-end failed to start or no channel
was accepted. Boot prints which channels the driver rejected, the mean
retune time and a `[spectrum] MHz` header with the channel centres. About
once per second the console prints two lines:

```text
[sweep] n=28 ms= 275 retune= 1489us gain=44a reg=2cc059de peak=5845 MHz level= 18.5dB env= 0.06 sync= 0.71 power= 15.20 clip= 0.0% strength=0.411 out=0.403 tone= 806 Hz duty=22% ring=04fec wovf=0 dump=80024000 rearm=0
[spectrum] dB      -    -    -    -    -    -    -    -    -    -    -    -    -    -    -    -    -    -    -    -    -    -    -    -    -   18    -    -
[peakwin] 5845 MHz oldest..newest:  16.6  17.3  16.6  15.2  15.3  15.6  15.5  16.2
[jammer] 1 channel above the noise rejected; strongest 5805 MHz level= 22.0dB env= 0.93 sync= 0.02
```

`n` is the number of channels measured, `ms` the sweep time, `retune` the
driver time of the last channel change, `gain` the forced PHY gain index
(`a` automatic, `m` manual), `reg` the raw gain register, `peak` the
strongest video channel with its `level` above the noise floor and its
discrimination scores `env` (envelope variance ratio) and `sync` (line
autocorrelation; -1 when the channel was too weak to test), `power` and
`clip` the highest minimum window power and clip share of the sweep on any
channel (AGC input), `strength` the level mapped to 0..1, `out` the smoothed
value driving the tone, `ring` the RX GDMA write offset (must keep changing;
`STALLED` marks a frozen ring, after 8 sweeps the firmware restarts itself),
`wovf` the sticky PARLIO RX FIFO overflow flag, `dump` the modem dump-engine
control register (bit 31 must stay set, otherwise MODEM_DIAG stops streaming
and the receiver is deaf) and `rearm` how often it had to be re-armed after a
retune. The `[spectrum]` line lists the level of every channel in dB (`-`
below 0.5 dB, `x` rejected by the driver, `J` after the level for a channel
that failed discrimination) in the order of the header; `[peakwin]` shows the
eight window powers of the peak channel's visit, oldest first (a retune
transient would show in the first ones), and `[jammer]` appears while a
rejected channel is above the noise.

Keys: `+` / `-` step the gain manually (leaves automatic mode), `a` returns
to automatic gain, `m` mutes the buzzer, `z` measures the idle floor of every
channel over ten sweeps (keep the transmitter off), `h` holds the receiver
on the current peak channel (press again to resume sweeping).

## Tuning

- `menuconfig` / `sdkconfig.defaults`: `CONFIG_C5VRX_BUZZER_SWEEP`,
  `CONFIG_C5VRX_BUZZER_SWEEP_MIN_MHZ` / `MAX_MHZ` / `STEP_MHZ`,
  `CONFIG_C5VRX_BUZZER_REJECT_NOISE`, `CONFIG_C5VRX_BUZZER_REQUIRE_SYNC`,
  `CONFIG_C5VRX_BUZZER_WIFI_CHANNEL` (start-up and fixed-mode channel),
  `CONFIG_C5VRX_BUZZER_GPIO`.
- `meter.c`: `DB_PER_INDEX`, `RANGE_DB`, `NOISE_POWER`, `SQUELCH_POWER`,
  `POWER_LOW` / `POWER_HIGH` (AGC window), `GAIN_MAX` (56; indices up to 54
  seen working on hardware, the register accepts higher values, the size of
  the PHY gain table is unknown; `reg` in the telemetry shows the raw
  register), `HISTORY` (visits per reported level), `SETTLE_TICKS`,
  `FLOOR_DEFAULT_DB`, discrimination `DISC_MIN_POWER`, `ENV_MAX_RATIO`,
  `SYNC_MIN_R`, `LINE_LAG_PAL` / `LINE_LAG_NTSC`.
- `main.c`: `TONE_MIN_HZ` / `TONE_MAX_HZ`, `DUTY_MIN` / `DUTY_MAX`,
  `SMOOTHING` (per sweep), `SILENT_BELOW`, `CAL_SWEEPS`.

## Status

Single-channel operation was calibrated on a XIAO ESP32-C5 with an external
antenna and a Raceband R6 (5843 MHz) transmitter 1 m away, channel 169,
20 s to 3.5 min per point:

| VTX power | level (median) | gain index | note |
|---|---|---|---|
| 25 mW | 14.3 dB (p10 12.8, p90 15.4) | 48..54 | steady |
| 200 mW | 24.6 dB (p10 23.3, p90 25.3) | 42 | steady, no clipping |
| 500 mW | 28.5 dB settled | 38 | the transmitter's output sagged to the 25 mW level for 1..3 s at a time (low battery), which the meter followed |

8x power read +10.3 dB (expected +9.0), 2.5x read +3.7 dB (expected +4.0),
so the meter is linear within about 1 dB over the range and `DB_PER_INDEX`
1.0 holds from gain 38 to 54. Idle is silent (gain 56, window power ~1).
Touching the board with the 25 mW transmitter reads 30+ dB.

The band sweep on the same board: all 28 standard channels are accepted by
the driver (5180..5885 MHz, channel 177 included), a retune takes 1.3..2.4 ms,
a full sweep 260..280 ms (about 3.7 sweeps/s), the dump engine never had to
be re-armed. The idle band reads 0 dB everywhere (window power ~2.0 at gain
56, Wi-Fi packets rejected by the minimum over the ring) and the buzzer is
silent. The 25 mW transmitter on 5843 MHz shows up on the 5845 MHz channel
only (neighbours 5825/5865 stay at 0 dB) at 15..20 dB, gain 44..50, and the
eight windows of a visit agree within about 1 dB, so the retune leaves no
transient in the ring. Switching the transmitter off returns every channel
to 0 dB and gain 56 within a few sweeps.

Two AGC rules came out of that run: a gain step down needs two consecutive
sweeps asking for it (one Wi-Fi packet filling a whole ring at maximum gain
used to bounce the gain by 6 and back), while a step up happens at once and
by 6 when the strongest channel fell below the squelch at the current gain,
because that channel reads 0 dB (tone off) until the gain catches up.

Not yet run on hardware: the jammer discrimination (the `env` and `sync`
scores of a real transmitter and of a jammer are the numbers to look at),
and the extra channels 146 (5730 MHz), 181 (5905 MHz) and 185 (5925 MHz)
offered to the driver for the Raceband R3 gap and the E8 / R8 frequencies.

Flashing note for this board: esptool's USB-JTAG reset did not start the
application after flashing and any host open/close of the port while the
application runs made the port unresponsive, so flash with the chip in the
ROM bootloader (hold BOOT while plugging in, or BOOT + RESET), press RESET
afterwards, and keep one serial session open. `tools/flash_when_ready.ps1`
does exactly that: it waits for the bootloader on the port, flashes the
latest build and starts `tools/serial_log.py`, a timestamping logger that
survives the re-enumeration on every reset and sends console keys from a
command file.
