/**
 * main.c - C5VRX-buzzer: 5.8 GHz FPV transmitter proximity buzzer.
 *
 *   RF: 5 GHz Wi-Fi front-end, BW40, AGC frozen, forced gain, retuned channel
 *       by channel across the band (rf.c)
 *     -> MODEM_DIAG Q[9:6]/I[9:6] -> PARLIO RX @ 40 MS/s -> 32 KiB ring (capture.c)
 *     -> per channel: minimum window power over the ring, level in dB,
 *        software AGC on the strongest channel -> strength 0..1 (meter.c)
 *     -> smoothing -> pitch and duty of an LEDC tone (buzzer.c)
 *
 * The closer the transmitter, the higher and louder the tone, whichever
 * channel it is on. Idle (maximum gain, no carrier anywhere) reads strength
 * 0 and is silent; the 'z' key measures per-channel idle floors if needed.
 * There is no CVBS/DAC output in this firmware.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "esp_system.h"

#include "buzzer.h"
#include "capture.h"
#include "meter.h"
#include "rf.h"

#define TELEMETRY_PERIOD_MS 1000u
#define STALL_SWEEPS        8u       /* ring write position unchanged for 8 sweeps: restart */

/* Tone mapping: strength 0..1 -> pitch (log scale) and duty. */
#define TONE_MIN_HZ 300.0f
#define TONE_MAX_HZ 3500.0f
#define DUTY_MIN    0.03f
#define DUTY_MAX    0.50f
#define SILENT_BELOW 0.01f   /* smoothed strength below this: buzzer off */
#define SMOOTHING   0.5f     /* exponential smoothing per sweep; 1 = none */

#define CAL_SWEEPS  10u      /* sweeps averaged by the 'z' key */

static bool  s_muted;
static volatile int s_last_peak = -1;   /* channel index of the last sweep's peak, for 'h' */

static void chirp(uint32_t hz1, uint32_t hz2, uint32_t ms)
{
    buzzer_tone(hz1, 0.3f);
    vTaskDelay(pdMS_TO_TICKS(ms));
    buzzer_tone(hz2, 0.3f);
    vTaskDelay(pdMS_TO_TICKS(ms));
    buzzer_off();
}

static void print_spectrum_header(void)
{
    unsigned count = 0u;
    const meter_channel_t *ch = meter_channels(&count);
    printf("[spectrum] MHz ");
    for (unsigned i = 0u; i < count; ++i)
        printf(" %4u%s", ch[i].mhz, ch[i].ok ? "" : "x");
    printf("\n");
    fflush(stdout);
}

static void print_status(const meter_sweep_t *s, float out, uint32_t tone_hz, float duty, unsigned stalled)
{
    printf("[sweep] n=%2u ms=%4lu retune=%5luus gain=%2u%s reg=%08lx peak=%4u MHz level=%5.1fdB env=%5.2f sync=%5.2f power=%6.2f clip=%4.1f%% strength=%.3f out=%.3f tone=%4lu Hz duty=%2u%% ring=%05lx%s wovf=%d dump=%08lx rearm=%lu%s%s%s\n",
           s->count, (unsigned long)((s->sweep_us + 500u) / 1000u), (unsigned long)s->retune_us,
           s->gain, meter_auto_gain() ? "a" : "m", (unsigned long)rf_get_rx_gain_reg(),
           s->peak_mhz, (double)s->peak_level_db, (double)s->peak_env, (double)s->peak_sync,
           (double)s->peak_power, (double)(s->peak_clip * 100.0f), (double)s->strength, (double)out,
           (unsigned long)tone_hz, (unsigned)(duty * 100.0f + 0.5f),
           (unsigned long)capture_write_offset(), stalled >= 2u ? " STALLED" : "",
           capture_rx_fifo_overflow() ? 1 : 0, (unsigned long)rf_get_dump_ctrl(),
           (unsigned long)rf_dump_rearm_count(),
           meter_hold() >= 0 ? " (hold)" : "", s_muted ? " (muted)" : "",
           s->calibrating ? " (calibrating)" : "");
    if (s->jammers > 0u) {
        printf("[jammer] %u channel%s above the noise rejected; strongest %u MHz level=%5.1fdB env=%5.2f sync=%5.2f\n",
               s->jammers, s->jammers == 1u ? "" : "s", s->jam_mhz, (double)s->jam_level_db,
               (double)s->jam_env, (double)s->jam_sync);
    }

    unsigned count = 0u;
    const meter_channel_t *ch = meter_channels(&count);
    printf("[spectrum] dB  ");
    for (unsigned i = 0u; i < count; ++i) {
        if (!ch[i].ok)
            printf("    x");
        else if (ch[i].level_db < 0.5f)
            printf("    -");
        else if (!ch[i].video)
            printf(" %3.0fJ", (double)ch[i].level_db);
        else
            printf(" %4.0f", (double)ch[i].level_db);
    }
    printf("\n");

    if (s->peak >= 0 && s->peak_level_db > 0.0f) {
        unsigned n = 0u;
        const float *w = meter_peak_windows(&n);
        printf("[peakwin] %u MHz oldest..newest:", s->peak_mhz);
        for (unsigned i = 0u; i < n; ++i) printf(" %5.1f", (double)w[i]);
        printf("\n");
    }
    fflush(stdout);
}

/* On-demand console: '+' / '-' manual gain, 'a' automatic gain, 'm' mute,
 * 'z' re-measure the idle floors, 'h' hold on / release the peak channel. */
static void console_task(void *arg)
{
    (void)arg;
    for (;;) {
        int c = getchar();
        if (c != EOF && c > 0) {
            if (c == '+' || c == 'k') {
                meter_nudge_gain(+2);
                printf("[gain] manual %u (reg=0x%08lx)\n", meter_gain(), (unsigned long)rf_get_rx_gain_reg());
            } else if (c == '-' || c == 'j') {
                meter_nudge_gain(-2);
                printf("[gain] manual %u (reg=0x%08lx)\n", meter_gain(), (unsigned long)rf_get_rx_gain_reg());
            } else if (c == 'a') {
                meter_set_auto_gain(true);
                printf("[gain] automatic\n");
            } else if (c == 'm') {
                s_muted = !s_muted;
                printf("[buzzer] %s\n", s_muted ? "muted" : "unmuted");
            } else if (c == 'z') {
                meter_calibrate_floor(CAL_SWEEPS);
                printf("[cal] measuring idle floors over %u sweeps, keep the transmitter off\n", (unsigned)CAL_SWEEPS);
            } else if (c == 'h') {
                if (meter_hold() >= 0) {
                    meter_set_hold(-1);
                    printf("[sweep] sweeping\n");
                } else if (s_last_peak < 0) {
                    printf("[sweep] no peak channel to hold (nothing above the noise)\n");
                } else {
                    meter_set_hold(s_last_peak);
                    unsigned n = 0u;
                    const meter_channel_t *ch = meter_channels(&n);
                    printf("[sweep] holding %u MHz\n", meter_hold() >= 0 ? ch[meter_hold()].mhz : 0u);
                }
            } else {
                printf("keys: '+'/'-' gain step, 'a' automatic gain, 'm' mute, 'z' re-zero idle floors, 'h' hold/release peak channel\n");
            }
            fflush(stdout);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(buzzer_init(CONFIG_C5VRX_BUZZER_GPIO));
    chirp(1000u, 2000u, 80u);   /* wiring check before RF bring-up */

    esp_err_t err = rf_start((uint8_t)CONFIG_C5VRX_BUZZER_WIFI_CHANNEL);
    if (err == ESP_OK) err = capture_start();
    if (err == ESP_OK) err = meter_init();
    if (err != ESP_OK) {
        for (int i = 0; i < 3; ++i) chirp(200u, 200u, 150u);   /* three low beeps: RF failed */
        ESP_ERROR_CHECK(err);
    }

    xTaskCreate(console_task, "console", 3072, NULL, 1, NULL);

    /* printf, not ESP_EARLY_LOG: the ROM printf cannot format floats. */
    printf("\n=======================================================\n"
           " C5VRX-buzzer: FPV transmitter proximity on GPIO%d\n"
           " Strength = frozen-AGC carrier power of the strongest swept channel\n"
           " Tone %.0f..%.0f Hz, duty %.0f..%.0f %%\n"
           "=======================================================\n",
           CONFIG_C5VRX_BUZZER_GPIO, (double)TONE_MIN_HZ, (double)TONE_MAX_HZ,
           (double)(DUTY_MIN * 100.0f), (double)(DUTY_MAX * 100.0f));
    print_spectrum_header();

    float smoothed = 0.0f;
    float out = 0.0f;
    unsigned stalled = 0u;
    uint32_t since_telemetry = TELEMETRY_PERIOD_MS;
    meter_sweep_t s = { 0 };

    for (;;) {
        s = meter_sweep();
        s_last_peak = (s.peak >= 0 && s.peak_level_db > 0.0f) ? s.peak : -1;

        /* Stall watchdog: the RX GDMA must move during every sweep. Seen on
         * hardware: after minutes of operation the ring stopped and the meter
         * kept reading one stale buffer (constant power at every gain). */
        stalled = s.ring_moved ? 0u : stalled + 1u;
        if (stalled >= STALL_SWEEPS) {
            printf("[capture] RX ring stalled for %u sweeps (offset %05lx, wovf=%d): restarting\n",
                   stalled, (unsigned long)capture_write_offset(),
                   capture_rx_fifo_overflow() ? 1 : 0);
            fflush(stdout);
            buzzer_off();
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
        }

        if (!s.calibrating)
            smoothed += SMOOTHING * (s.strength - smoothed);
        out = smoothed;
        if (out < 0.0f) out = 0.0f;
        if (out > 1.0f) out = 1.0f;

        uint32_t tone_hz = 0u;
        float duty = 0.0f;
        if (!s_muted && !s.calibrating && out > SILENT_BELOW) {
            tone_hz = (uint32_t)(TONE_MIN_HZ * powf(TONE_MAX_HZ / TONE_MIN_HZ, out));
            duty = DUTY_MIN + (DUTY_MAX - DUTY_MIN) * out;
            buzzer_tone(tone_hz, duty);
        } else {
            buzzer_off();
        }

        since_telemetry += (s.sweep_us + 500u) / 1000u;
        if (since_telemetry >= TELEMETRY_PERIOD_MS) {
            since_telemetry = 0u;
            print_status(&s, out, tone_hz, duty, stalled);
        }
        vTaskDelay(1);   /* let the console task run between sweeps */
    }
}
