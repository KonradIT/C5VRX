/**
 * meter.c - Received carrier strength per channel, swept over the 5 GHz band,
 * with jammer discrimination.
 *
 * Why not the demodulated video? FM demodulation removes amplitude by design:
 * the recovered CVBS has the same swing whether the transmitter is 1 m or
 * 100 m away, right up to the point where it dissolves into noise. What does
 * track distance is the received carrier power, i.e. the magnitude of the I/Q
 * samples before demodulation. With the hardware AGC frozen (rf.c) that
 * magnitude is proportional to the RF input for a given forced gain index.
 *
 * The 4-bit samples only cover ~24 dB, so a software AGC keeps the power
 * inside a window by stepping the forced gain index: strong signal -> low
 * gain index, weak signal -> high gain index. One gain index serves the whole
 * sweep and follows the strongest channel; weaker channels then simply read
 * "nothing above the noise" at that gain, which is fine because only the
 * strongest channel drives the buzzer. The level of a channel is its carrier
 * power in dB above the idle noise floor measured at maximum gain:
 *
 *   level_db = (GAIN_MAX - gain) * DB_PER_INDEX + 10 log10(power / NOISE_POWER)
 *   strength = max over video channels (level_db) / RANGE_DB
 *
 * DB_PER_INDEX is the gain change of one PHY gain index (measured on
 * hardware, see README), so levels taken at different gains are comparable
 * and the per-channel history survives gain steps.
 *
 * The band is shared with 5 GHz Wi-Fi, whose packets raise the power of
 * single windows by 10 dB or more. Each visit therefore takes the MINIMUM
 * window power over the whole 819 us ring (a packet shorter than that leaves
 * at least one quiet window), and the reported level is the minimum over the
 * last HISTORY visits: a continuous FM carrier keeps those minima up, bursts
 * do not.
 *
 * Jammer discrimination, on every channel strong enough to judge:
 *   1. Envelope. An FM carrier has a constant envelope, so the per-sample
 *      power I^2 + Q^2 barely varies (variance / mean^2 of a few percent,
 *      quantisation only). Gaussian noise has an exponential power
 *      distribution, variance / mean^2 = 1. A pure-noise jammer fails here.
 *   2. Line sync. The FM discriminator (cross product of consecutive I/Q
 *      samples) recovers the composite video; its autocorrelation at one
 *      television line (64.0 us PAL, 63.6 us NTSC) is high because every
 *      line starts with the same sync pulse and blanking. An unmodulated
 *      carrier, a noise-modulated carrier or a swept jammer has none.
 * A channel drives the buzzer only if it passed in this or the previous
 * visit (or is too weak to test). The AGC still follows raw power, so a
 * strong jammer lowers the gain and masks weaker transmitters: that is the
 * physics of one front-end, not a policy.
 */

#include "meter.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "capture.h"
#include "rf.h"

#define WINDOW_BYTES  4096u   /* samples per window (102.4 us); 8 windows cover the ring */
#define WINDOWS       (CAPTURE_RING_BYTES / WINDOW_BYTES)
#define HISTORY       2u      /* visits per reported level (one sweep of latency) */
#define SETTLE_TICKS  2u      /* >= 1 ms after a retune: PLL settled, ring (819 us) fully rewritten */

#define GAIN_MIN   0
#define GAIN_MAX   56         /* PHY table size unknown; 54 seen working on hardware, 56 adds reach (register takes it) */
#define GAIN_STEP  2

#define POWER_LOW  6.0f       /* strongest channel below: raise gain */
#define POWER_HIGH 40.0f      /* strongest channel above: lower gain */
#define CLIP_MAX   0.02f      /* every window of a channel >2 % full scale: lower gain */
#define CLIP_FAST  0.20f      /* heavy clipping: take a big step down */

#define NOISE_POWER   2.0f    /* idle floor at GAIN_MAX: ~1.0 quantisation offset + ~1.0 thermal noise */
#define SQUELCH_POWER 3.0f    /* below this the channel holds nothing above the noise at the current gain */
#define DB_PER_INDEX  1.0f    /* measured 2026-09-18: 25/200/500 mW at 1 m read 14.3/24.6/28.5 dB (gains 54..38) */
#define RANGE_DB      45.0f   /* level mapped to full scale; 25 mW at 1 m reads ~14 dB, touching the board ~30+ dB */

#define FLOOR_DEFAULT_DB 1.0f /* idle level a channel may show before calibration */
#define CAL_MARGIN_DB    1.0f
#define CAL_FLOOR_MAX_DB 20.0f

/* Discrimination. Tested only when the minimum window power is at least
 * DISC_MIN_POWER (>= 6 dB above the noise at maximum gain, or well inside
 * the AGC window at lower gains). */
#define DISC_MIN_POWER 6.0f
#define ENV_MAX_RATIO  0.7f   /* FM carrier: <0.1 clean, ~0.4 at 6 dB SNR; Gaussian noise: ~1.0 (0.8 with mild clipping) */
#define SYNC_MIN_R     0.25f  /* video: >0.5; noise, CW, swept: |r| < 0.1 */
#define DISC_BLOCKS    6u     /* ring blocks snapshotted for the discriminator (614 us, 9.6 PAL lines) */
#define DISC_DECIM     32u    /* discriminator output decimation: 40 MS/s -> 1.25 MS/s */
#define DISC_SAMPLES   (DISC_BLOCKS * WINDOW_BYTES / DISC_DECIM)   /* 768 */
#define LINE_LAG_PAL   80u    /* 64.0 us at 1.25 MS/s */
#define LINE_LAG_NTSC  79u    /* 63.6 us -> 79.4 samples */

static const int8_t k_sign4[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1
};
static int8_t   k_i[256];     /* I[9:6] of a (I[9:6] << 4 | Q[9:6]) byte */
static int8_t   k_q[256];
static uint8_t  k_pow[256];   /* I^2 + Q^2, 0..128 */
static uint16_t k_pow2[256];  /* (I^2 + Q^2)^2 */
static uint8_t  k_clip[256];  /* 1 if I or Q sits at full scale */

typedef struct {
    float    hist[HISTORY];
    unsigned hist_len;
    unsigned hist_pos;
    float    cal_max;
    bool     prev_pass;   /* previous visit was tested and passed discrimination */
} channel_state_t;

static meter_channel_t s_ch[METER_MAX_CHANNELS];
static channel_state_t s_st[METER_MAX_CHANNELS];
static unsigned s_count;
static int      s_hold = -1;
static uint8_t  s_gain = RF_INITIAL_GAIN;
static bool     s_auto = true;
static unsigned s_cal_left;
static int      s_agc_wanted;   /* direction the previous sweep asked for: -1, 0, +1 */
static float    s_peak_windows[WINDOWS];   /* window powers of the peak channel, oldest first */

/* Discriminator scratch: a time-contiguous snapshot of the ring and the
 * decimated instantaneous frequency. */
static uint8_t  s_snap[DISC_BLOCKS * WINDOW_BYTES];
static int16_t  s_dec[DISC_SAMPLES];

static void apply_gain(int gain)
{
    if (gain < GAIN_MIN) gain = GAIN_MIN;
    if (gain > GAIN_MAX) gain = GAIN_MAX;
    s_gain = (uint8_t)gain;
    rf_set_rx_gain(true, s_gain);
    printf("[gain] %u reg=%08lx\n", s_gain, (unsigned long)rf_get_rx_gain_reg());
}

static void add_channel(uint8_t channel)
{
    if (s_count >= METER_MAX_CHANNELS) return;
    meter_channel_t *c = &s_ch[s_count++];
    memset(c, 0, sizeof(*c));
    c->channel  = channel;
    c->mhz      = (uint16_t)(5000u + 5u * (unsigned)channel);
    c->floor_db = FLOOR_DEFAULT_DB;
    c->sync_r   = -1.0f;
    c->video    = true;
}

/* Standard 5 GHz grid, 20 MHz apart: 36..64 and 100..144 (multiples of 4),
 * 149..177 (1 mod 4). 181 and 185 (5905, 5925 MHz: FPV bands E8 and R8) and
 * 146 (5730 MHz: the 25 MHz gap around Raceband R3) are offered to the
 * driver as well; meter_init() drops what it rejects. Filtered by the
 * Kconfig range and stride. */
static void build_channel_list(void)
{
    s_count = 0u;
#if CONFIG_C5VRX_BUZZER_SWEEP
    static const uint8_t grid[] = {
        36, 40, 44, 48, 52, 56, 60, 64,
        100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 146,
        149, 153, 157, 161, 165, 169, 173, 177, 181, 185,
    };
    const unsigned stride = (unsigned)CONFIG_C5VRX_BUZZER_SWEEP_STEP_MHZ / 20u;
    unsigned index = 0u;
    for (unsigned n = 0u; n < sizeof(grid) / sizeof(grid[0]); ++n) {
        unsigned mhz = 5000u + 5u * grid[n];
        if (mhz < (unsigned)CONFIG_C5VRX_BUZZER_SWEEP_MIN_MHZ ||
            mhz > (unsigned)CONFIG_C5VRX_BUZZER_SWEEP_MAX_MHZ)
            continue;
        if (grid[n] == 146u) { add_channel(grid[n]); continue; }   /* gap filler, never strided out */
        if ((index++ % stride) == 0u) add_channel(grid[n]);
    }
#endif
    if (s_count == 0u) add_channel((uint8_t)CONFIG_C5VRX_BUZZER_WIFI_CHANNEL);
}

esp_err_t meter_init(void)
{
    for (unsigned b = 0u; b < 256u; ++b) {
        int q = k_sign4[b & 0x0fu];
        int i = k_sign4[b >> 4];
        int p = q * q + i * i;
        k_i[b]    = (int8_t)i;
        k_q[b]    = (int8_t)q;
        k_pow[b]  = (uint8_t)p;
        k_pow2[b] = (uint16_t)(p * p);
        k_clip[b] = (q == 7 || q == -8 || i == 7 || i == -8) ? 1u : 0u;
    }

    build_channel_list();
    s_auto = true;
    s_hold = -1;
    apply_gain(GAIN_MAX);   /* idle is silent from the first sweep; a carrier steps it down */

    /* Ask the driver for every channel once; keep what it accepts. */
    unsigned accepted = 0u;
    uint64_t retune_sum = 0u;
    for (unsigned i = 0u; i < s_count; ++i) {
        esp_err_t err = rf_set_channel(s_ch[i].channel, s_gain);
        s_ch[i].ok = (err == ESP_OK);
        if (s_ch[i].ok) {
            accepted++;
            retune_sum += rf_last_retune_us();
        } else {
            printf("[sweep] channel %u (%u MHz) rejected by the driver: %s\n",
                   s_ch[i].channel, s_ch[i].mhz, esp_err_to_name(err));
        }
    }
    if (accepted == 0u) {
        /* The start-up channel is proven; fall back to it alone. */
        s_count = 0u;
        add_channel((uint8_t)CONFIG_C5VRX_BUZZER_WIFI_CHANNEL);
        s_ch[0].ok = (rf_set_channel(s_ch[0].channel, s_gain) == ESP_OK);
        if (!s_ch[0].ok) return ESP_ERR_NOT_FOUND;
        accepted = 1u;
        retune_sum = rf_last_retune_us();
    }
    printf("[sweep] %u of %u channels accepted, %u..%u MHz, retune %lu us each; discrimination: envelope %s, line sync %s\n",
           accepted, s_count, s_ch[0].mhz, s_ch[s_count - 1u].mhz,
           (unsigned long)(retune_sum / accepted),
           CONFIG_C5VRX_BUZZER_REJECT_NOISE ? "on" : "off",
           CONFIG_C5VRX_BUZZER_REQUIRE_SYNC ? "on" : "off");
    fflush(stdout);
    return ESP_OK;
}

const meter_channel_t *meter_channels(unsigned *count)
{
    if (count) *count = s_count;
    return s_ch;
}

void meter_set_auto_gain(bool enable)
{
    s_auto = enable;
}

bool meter_auto_gain(void)
{
    return s_auto;
}

void meter_nudge_gain(int delta)
{
    s_auto = false;
    apply_gain((int)s_gain + delta);
}

uint8_t meter_gain(void)
{
    return s_gain;
}

void meter_calibrate_floor(unsigned sweeps)
{
    for (unsigned i = 0u; i < s_count; ++i) s_st[i].cal_max = 0.0f;
    s_cal_left = sweeps;
}

void meter_set_hold(int channel_index)
{
    s_hold = (channel_index >= 0 && (unsigned)channel_index < s_count && s_ch[channel_index].ok)
             ? channel_index : -1;
}

int meter_hold(void)
{
    return s_hold;
}

const float *meter_peak_windows(unsigned *count)
{
    if (count) *count = WINDOWS;
    return s_peak_windows;
}

/* Minimum and maximum window power, minimum clip ratio and minimum envelope
 * variance ratio over the ring. The DMA keeps writing while we read; a window
 * torn between two ring passes is still post-retune data, and the minima
 * tolerate it. */
static void measure_ring(meter_channel_t *c, float *windows)
{
    const uint8_t *ring = capture_ring();
    /* Oldest data sits just ahead of the DMA write position. */
    unsigned first = (capture_write_offset() / WINDOW_BYTES + 1u) % WINDOWS;
    float pmin = 1e9f, pmax = 0.0f, cmin = 1.0f, emin = 1e9f;
    for (unsigned k = 0u; k < WINDOWS; ++k) {
        unsigned w = (first + k) % WINDOWS;
        const uint8_t *p = ring + w * WINDOW_BYTES;
        uint32_t sum = 0u, sumsq = 0u, clipped = 0u;
        for (uint32_t n = 0u; n < WINDOW_BYTES; n += 4u) {
            uint8_t b0 = p[n], b1 = p[n + 1u], b2 = p[n + 2u], b3 = p[n + 3u];
            sum     += k_pow[b0] + k_pow[b1] + k_pow[b2] + k_pow[b3];
            sumsq   += (uint32_t)k_pow2[b0] + k_pow2[b1] + k_pow2[b2] + k_pow2[b3];
            clipped += k_clip[b0] + k_clip[b1] + k_clip[b2] + k_clip[b3];
        }
        float mean  = (float)sum / (float)WINDOW_BYTES;
        float msq   = (float)sumsq / (float)WINDOW_BYTES;
        float clip  = (float)clipped / (float)WINDOW_BYTES;
        float ratio = mean > 0.5f ? (msq - mean * mean) / (mean * mean) : 1.0f;
        windows[k] = mean;
        if (mean < pmin) pmin = mean;
        if (mean > pmax) pmax = mean;
        if (clip < cmin) cmin = clip;
        if (ratio < emin) emin = ratio;
    }
    c->power_min = pmin;
    c->power_max = pmax;
    c->clip_min  = cmin;
    c->env_ratio = emin;
}

/* Normalised autocorrelation of the FM-demodulated signal at one television
 * line. Snapshots six ring blocks in time order, skipping the block being
 * written and the one the DMA reaches next (the copy runs ~4x faster than
 * the DMA, so the snapshot stays ahead of it), demodulates with the cross
 * product I[n-1] Q[n] - Q[n-1] I[n] (proportional to sin of the phase step,
 * monotonic for the deviations of FPV video at 40 MS/s), sums 32 samples
 * per output and correlates at the PAL and NTSC line lags. */
static float line_sync_score(void)
{
    const uint8_t *ring = capture_ring();
    unsigned w = capture_write_offset() / WINDOW_BYTES;
    for (unsigned k = 0u; k < DISC_BLOCKS; ++k)
        memcpy(s_snap + k * WINDOW_BYTES, ring + ((w + 2u + k) % WINDOWS) * WINDOW_BYTES, WINDOW_BYTES);

    int prev_i = k_i[s_snap[0]], prev_q = k_q[s_snap[0]];
    for (unsigned n = 0u; n < DISC_SAMPLES; ++n) {
        const uint8_t *p = s_snap + n * DISC_DECIM;
        int32_t acc = 0;
        for (unsigned m = 0u; m < DISC_DECIM; ++m) {
            int i = k_i[p[m]], q = k_q[p[m]];
            acc += prev_i * q - prev_q * i;
            prev_i = i;
            prev_q = q;
        }
        s_dec[n] = (int16_t)acc;
    }

    float mean = 0.0f;
    for (unsigned n = 0u; n < DISC_SAMPLES; ++n) mean += (float)s_dec[n];
    mean /= (float)DISC_SAMPLES;

    float energy = 0.0f, c_pal = 0.0f, c_ntsc = 0.0f;
    for (unsigned n = 0u; n < DISC_SAMPLES; ++n) {
        float x = (float)s_dec[n] - mean;
        energy += x * x;
        if (n + LINE_LAG_PAL < DISC_SAMPLES)
            c_pal += x * ((float)s_dec[n + LINE_LAG_PAL] - mean);
        if (n + LINE_LAG_NTSC < DISC_SAMPLES)
            c_ntsc += x * ((float)s_dec[n + LINE_LAG_NTSC] - mean);
    }
    if (energy <= 0.0f) return 0.0f;
    float r = (c_pal > c_ntsc ? c_pal : c_ntsc) / energy;
    return r;
}

/* Carrier level above the idle noise floor for a window power at a gain. */
static float level_from(float power_min, uint8_t gain)
{
    if (power_min < SQUELCH_POWER) return 0.0f;   /* nothing above the noise at this gain */
    float p = power_min > NOISE_POWER ? power_min : NOISE_POWER;
    return (float)(GAIN_MAX - (int)gain) * DB_PER_INDEX + 10.0f * log10f(p / NOISE_POWER);
}

meter_sweep_t meter_sweep(void)
{
    meter_sweep_t s = { 0 };
    s.peak = -1;
    s.gain = s_gain;
    s.peak_env = -1.0f;
    s.peak_sync = -1.0f;
    s.jam_env = -1.0f;
    s.jam_sync = -1.0f;
    s.calibrating = s_cal_left > 0u;

    int64_t  t0   = esp_timer_get_time();
    uint32_t off0 = capture_write_offset();
    unsigned first = 0u, last = s_count;
    if (s_hold >= 0) {
        first = (unsigned)s_hold;
        last  = first + 1u;
    }

    float best_level = -1.0f;
    float best_jam   = 0.0f;
    for (unsigned i = first; i < last; ++i) {
        meter_channel_t *c = &s_ch[i];
        channel_state_t *st = &s_st[i];
        if (!c->ok) continue;

        if (rf_channel() != c->channel && rf_set_channel(c->channel, s_gain) != ESP_OK)
            continue;   /* transient driver refusal: skip this visit */
        vTaskDelay(SETTLE_TICKS);
        if (capture_write_offset() != off0) s.ring_moved = true;

        float windows[WINDOWS];
        measure_ring(c, windows);
        c->level_raw_db = level_from(c->power_min, s_gain);

        /* Discrimination on channels strong enough to judge. */
        c->tested = c->power_min >= DISC_MIN_POWER;
        c->sync_r = -1.0f;
        bool pass = true;
        if (c->tested) {
#if CONFIG_C5VRX_BUZZER_REJECT_NOISE
            if (c->env_ratio > ENV_MAX_RATIO) pass = false;
#endif
#if CONFIG_C5VRX_BUZZER_REQUIRE_SYNC
            c->sync_r = line_sync_score();
            if (c->sync_r < SYNC_MIN_R) pass = false;
#endif
        }
        c->video = !c->tested || pass || st->prev_pass;
        st->prev_pass = c->tested && pass;

        st->hist[st->hist_pos] = c->level_raw_db;
        st->hist_pos = (st->hist_pos + 1u) % HISTORY;
        if (st->hist_len < HISTORY) st->hist_len++;
        float m = st->hist[0];
        for (unsigned n = 1u; n < st->hist_len; ++n)
            if (st->hist[n] < m) m = st->hist[n];
        if (s_cal_left > 0u && c->level_raw_db > st->cal_max) st->cal_max = c->level_raw_db;

        float lvl = m - c->floor_db;
        c->level_db = lvl > 0.0f ? lvl : 0.0f;
        s.count++;

        if (c->video) {
            if (c->level_db > best_level) {
                best_level = c->level_db;
                s.peak = (int)i;
                s.peak_env = c->tested ? c->env_ratio : -1.0f;
                s.peak_sync = c->sync_r;
                memcpy(s_peak_windows, windows, sizeof(s_peak_windows));
            }
        } else if (c->level_db > 0.0f) {
            s.jammers++;
            if (c->level_db > best_jam) {
                best_jam = c->level_db;
                s.jam_mhz = c->mhz;
                s.jam_level_db = c->level_db;
                s.jam_env = c->env_ratio;
                s.jam_sync = c->sync_r;
            }
        }
        if (c->power_min > s.peak_power) s.peak_power = c->power_min;
        if (c->clip_min > s.peak_clip) s.peak_clip = c->clip_min;
    }

    if (s.peak >= 0) {
        s.peak_mhz      = s_ch[s.peak].mhz;
        s.peak_level_db = best_level;
        s.strength      = best_level / RANGE_DB;
        if (s.strength > 1.0f) s.strength = 1.0f;
    }
    s.sweep_us  = (uint32_t)(esp_timer_get_time() - t0);
    s.retune_us = rf_last_retune_us();

    if (s_cal_left > 0u && --s_cal_left == 0u) {
        for (unsigned i = 0u; i < s_count; ++i) {
            float f = s_st[i].cal_max + CAL_MARGIN_DB;
            if (f > CAL_FLOOR_MAX_DB) f = CAL_FLOOR_MAX_DB;
            s_ch[i].floor_db = f;
        }
        printf("[cal] idle floor per channel set (first channel max %.1f dB + %.1f dB margin)\n",
               (double)s_st[0].cal_max, (double)CAL_MARGIN_DB);
        fflush(stdout);
    }

    /* Software AGC on the strongest channel (video or not): a step down is
     * taken only when two consecutive sweeps ask for it, so a single Wi-Fi
     * burst that fills a whole ring cannot move the gain; a step up happens
     * at once, and by 6 while the strongest channel sits below the squelch at
     * this gain (it reads 0 dB, tone off, until the gain catches up). Bigger
     * steps while the ADC clips hard. Levels are gain-normalised, so the
     * histories are kept. */
    int wanted = 0;
    if ((s.peak_clip > CLIP_MAX || s.peak_power > POWER_HIGH) && s_gain > GAIN_MIN)
        wanted = -1;
    else if (s.peak_power < POWER_LOW && s_gain < GAIN_MAX)
        wanted = +1;
    if (s_auto && s.count > 0u && wanted < 0 && s_agc_wanted < 0) {
        int step = (s.peak_clip > CLIP_FAST) ? 3 * GAIN_STEP : GAIN_STEP;
        apply_gain((int)s_gain - step);
        wanted = 0;
    } else if (s_auto && s.count > 0u && wanted > 0) {
        int step = (s.peak_power < SQUELCH_POWER) ? 3 * GAIN_STEP
                 : (s.peak_power < POWER_LOW / 2.0f) ? 2 * GAIN_STEP : GAIN_STEP;
        apply_gain((int)s_gain + step);
        wanted = 0;
    }
    s_agc_wanted = wanted;

    return s;
}
