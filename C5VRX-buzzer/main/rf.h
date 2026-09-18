#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

/* Number of MODEM_DIAG lanes captured by PARLIO RX: Q[9:6] then I[9:6]. */
#define RF_IQ_LANES 8u

/* Forced PHY receiver gain index applied by rf_start(); the meter's software
 * AGC moves it afterwards (see meter.c). */
#define RF_INITIAL_GAIN 24u

/**
 * rf_start() - Initialize the ESP32-C5 Wi-Fi/PHY receive-only front-end on
 * one Wi-Fi channel (5000 + 5 * channel MHz).
 *
 * Identical RF configuration to C5VRX-3 rf.c except for the channel:
 *   - 5 GHz band only, BW40, no BW20 fallback
 *   - no power saving, promiscuous RX keeps MODEM_DIAG clocking
 *   - all LMAC TX queues hardware-disabled (receive-only)
 *   - hardware AGC frozen, BW20 analog filter, fixed gain RF_INITIAL_GAIN
 *   - MODEM_DIAG DIAG[6:9] (Q[9:6]) and DIAG[16:19] (I[9:6]) routed to the
 *     lane GPIOs returned by rf_iq_lane_gpios()
 */
esp_err_t rf_start(uint8_t channel);

/**
 * rf_set_channel() - Retune to another Wi-Fi channel and restore the
 * receive state the driver's channel switch may disturb: frozen AGC, BW20
 * analog filter, forced gain index, disabled TX queues, armed dump engine.
 * Blocks for the driver's retune time (see rf_last_retune_us()). Returns the
 * driver's error for channels it does not accept; the receiver then stays on
 * the previous channel.
 */
esp_err_t rf_set_channel(uint8_t channel, uint8_t gain_idx);

/** Wi-Fi channel the receiver is tuned to. */
uint8_t rf_channel(void);

/** Duration of the last successful esp_wifi_set_channel() call, microseconds. */
uint32_t rf_last_retune_us(void);

/** How often the modem dump engine had to be re-armed after a retune. */
uint32_t rf_dump_rearm_count(void);

/**
 * GPIO carrying MODEM_DIAG lane 0..7 (Q[6], Q[7], Q[8], Q[9], I[6], I[7],
 * I[8], I[9]). PARLIO RX must list the same GPIOs in the same order, so that
 * a captured byte is (I[9:6] << 4) | Q[9:6].
 */
const gpio_num_t *rf_iq_lane_gpios(void);

/**
 * Force the PHY receiver gain index (0 = minimum gain / maximum attenuation,
 * ~30-60 = high gain). force = false restores the PHY default behaviour.
 */
void rf_set_rx_gain(bool force, uint8_t gain_idx);

/** Raw PHY gain register, for telemetry only. */
uint32_t rf_get_rx_gain_reg(void);

/** Raw modem dump-engine control register (DUMP_CTRL); bit 31 = ENABLE. The
 * engine keeps MODEM_DIAG streaming; if it disarms, the receiver goes deaf. */
uint32_t rf_get_dump_ctrl(void);
