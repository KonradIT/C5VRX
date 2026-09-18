#pragma once

#include <stdint.h>

#include "esp_err.h"

/** Configure LEDC PWM on the buzzer GPIO. The buzzer stays silent. */
esp_err_t buzzer_init(int gpio);

/**
 * Play a tone. duty is the PWM duty cycle 0..0.5 (0.5 = square wave, the
 * loudest a passive piezo gets; 0 silences). freq_hz is clamped to
 * 100..20000 Hz.
 */
void buzzer_tone(uint32_t freq_hz, float duty);

void buzzer_off(void);
