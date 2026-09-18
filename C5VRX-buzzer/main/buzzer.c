/**
 * buzzer.c - Passive buzzer tone on one GPIO through LEDC PWM.
 *
 * ESP32-C5 LEDC has low-speed mode only. One timer (10-bit resolution) sets
 * the pitch, one channel sets the duty cycle. For a passive piezo the sound
 * pressure rises with duty up to 50 % (square wave); an active buzzer just
 * buzzes at its own pitch whenever the duty is above zero.
 */

#include "buzzer.h"

#include "driver/ledc.h"

#define BUZZER_MODE    LEDC_LOW_SPEED_MODE
#define BUZZER_TIMER   LEDC_TIMER_0
#define BUZZER_CHANNEL LEDC_CHANNEL_0
#define BUZZER_RES     LEDC_TIMER_10_BIT
#define BUZZER_DUTY_MAX ((1u << 10) - 1u)

#define FREQ_MIN_HZ 100u
#define FREQ_MAX_HZ 20000u

static uint32_t s_freq_hz;

esp_err_t buzzer_init(int gpio)
{
    const ledc_timer_config_t timer = {
        .speed_mode      = BUZZER_MODE,
        .duty_resolution = BUZZER_RES,
        .timer_num       = BUZZER_TIMER,
        .freq_hz         = 1000u,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    s_freq_hz = timer.freq_hz;

    const ledc_channel_config_t channel = {
        .gpio_num   = gpio,
        .speed_mode = BUZZER_MODE,
        .channel    = BUZZER_CHANNEL,
        .timer_sel  = BUZZER_TIMER,
        .duty       = 0u,
        .hpoint     = 0,
    };
    return ledc_channel_config(&channel);
}

void buzzer_tone(uint32_t freq_hz, float duty)
{
    if (freq_hz == 0u || duty <= 0.0f) {
        buzzer_off();
        return;
    }
    if (freq_hz < FREQ_MIN_HZ) freq_hz = FREQ_MIN_HZ;
    if (freq_hz > FREQ_MAX_HZ) freq_hz = FREQ_MAX_HZ;
    if (duty > 0.5f) duty = 0.5f;

    if (freq_hz != s_freq_hz) {
        if (ledc_set_freq(BUZZER_MODE, BUZZER_TIMER, freq_hz) == ESP_OK)
            s_freq_hz = freq_hz;
    }
    (void)ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, (uint32_t)(duty * (float)BUZZER_DUTY_MAX));
    (void)ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
}

void buzzer_off(void)
{
    (void)ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0u);
    (void)ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
}
