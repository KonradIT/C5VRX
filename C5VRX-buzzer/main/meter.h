#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Upper bound on swept channels: the 5 GHz Wi-Fi grid has 28 standard 20 MHz
 * channels (36..64, 100..144, 149..177) plus the candidates 146, 181 and 185
 * that the driver may or may not accept. */
#define METER_MAX_CHANNELS 40u

/* One swept channel. power_*, clip_min, env_ratio and sync_r are from the
 * latest visit; level_db and video are what the buzzer uses. */
typedef struct {
    uint8_t  channel;      /* Wi-Fi channel number, centre 5000 + 5 * channel MHz */
    uint16_t mhz;
    bool     ok;           /* accepted by the Wi-Fi driver at meter_init() */
    float    power_min;    /* minimum window power over the ring (bursts rejected), 4-bit LSB^2 */
    float    power_max;    /* maximum window power over the ring */
    float    clip_min;     /* minimum window clip ratio over the ring */
    float    env_ratio;    /* envelope variance / mean^2, minimum over windows: ~0 FM carrier, ~1 noise */
    float    sync_r;       /* FM-demodulated line-rate autocorrelation: >0.5 video, ~0 noise or CW; -1 not measured */
    bool     tested;       /* strong enough this visit for the discrimination tests */
    bool     video;        /* passes discrimination (or too weak to test): may drive the buzzer */
    float    level_raw_db; /* carrier level above the noise floor at the sweep gain; 0 = nothing */
    float    level_db;     /* reported: minimum level of the last sweeps minus the idle floor, >= 0 */
    float    floor_db;     /* idle floor from calibration (subtracted) */
} meter_channel_t;

/* Result of one pass over all channels. */
typedef struct {
    unsigned count;         /* channels measured in this sweep */
    uint8_t  gain;          /* forced PHY gain index during the sweep */
    int      peak;          /* index of the strongest video channel, -1 if none */
    uint16_t peak_mhz;
    float    peak_level_db; /* reported level of the strongest video channel */
    float    strength;      /* peak_level_db / RANGE_DB clamped to 0..1 */
    float    peak_env;      /* discrimination scores of the peak channel (-1 = not tested) */
    float    peak_sync;
    float    peak_power;    /* highest power_min of the sweep, any channel (software AGC input) */
    float    peak_clip;     /* highest clip_min of the sweep */
    unsigned jammers;       /* channels above the noise that failed discrimination */
    uint16_t jam_mhz;       /* strongest of them */
    float    jam_level_db;
    float    jam_env;
    float    jam_sync;
    uint32_t sweep_us;      /* wall time of the sweep */
    uint32_t retune_us;     /* driver time of the last channel change */
    bool     ring_moved;    /* the RX GDMA write offset changed during the sweep */
    bool     calibrating;   /* idle-floor calibration in progress: ignore strength */
} meter_sweep_t;

/**
 * Build the channel list from Kconfig (sweep range or the single fixed
 * channel), ask the driver to tune each one once and drop the rejected ones,
 * then park the gain at maximum. Requires rf_start() and capture_start().
 * Fails only if no channel at all is accepted.
 */
esp_err_t meter_init(void);

/**
 * One sweep: tune every accepted channel, wait for the ring to refill, take
 * the minimum window power over the ring, run the jammer discrimination on
 * channels strong enough for it, update the per-channel level and run the
 * software AGC once on the strongest channel. Blocks for the sweep time
 * (roughly 2 ms per channel plus the driver's retune time, plus ~2 ms per
 * channel above the noise).
 */
meter_sweep_t meter_sweep(void);

/** The channel table (count entries, some may have ok == false). */
const meter_channel_t *meter_channels(unsigned *count);

void    meter_set_auto_gain(bool enable);
bool    meter_auto_gain(void);
/** Manual gain step (disables automatic mode). */
void    meter_nudge_gain(int delta);
uint8_t meter_gain(void);

/** Measure the idle floor of every channel over the next sweeps (transmitter
 * off) and subtract it from the reported levels afterwards. */
void meter_calibrate_floor(unsigned sweeps);

/** Restrict the sweep to one channel index (-1 = sweep everything). */
void meter_set_hold(int channel_index);
int  meter_hold(void);

/** Window powers of the last sweep's peak channel, oldest first (diagnostic:
 * a retune transient shows up as the first windows differing from the rest). */
const float *meter_peak_windows(unsigned *count);
