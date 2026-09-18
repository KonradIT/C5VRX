/**
 * rf.c - ESP32-C5 Wi-Fi/PHY receive-only front-end initialization.
 *
 * Derived from C5VRX-3 rf.c (proven MODEM_DIAG Q4/I4 capture at 5865 MHz /
 * BW40). Two things differ from C5VRX-3:
 *
 *   1. Lane GPIOs. C5VRX-3 drives Q[8] on GPIO25 (XIAO pad D2) and I[7] on
 *      GPIO3 (XIAO pad A2/MTDI). This firmware needs one of those pads for the
 *      buzzer, so both lanes moved to the former CVBS DAC pads GPIO23 (D4)
 *      and GPIO24 (D5). Every lane is a plain GPIO-matrix route, so any output
 *      capable GPIO works; the order of the table is what matters.
 *   2. The Wi-Fi vendor timer inventory (OSI hooks) is gone; it only served
 *      the video receiver's periodic-stall investigation.
 *
 * IMPORTANT: BW40 failure returns an error. NO BW20 fallback.
 */

#include "rf.h"

#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_gpio.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "soc/gpio_sig_map.h"
#include "modem/modem_syscon_reg.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "heap_memory_layout.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "sdkconfig.h"

/* Reserve the RF dump memory bank (0x4082ffc0..0x40850040) from the heap.
 * The modem dump engine continuously streams 40 MS/s IQ words into 0x40830000.
 * Reserving this region ensures FreeRTOS stacks, Wi-Fi buffers, and GDMA descriptors
 * are never allocated in this address space. */
#define PRE_GUARD_ADDR  0x4082ffc0u
#define POST_GUARD_ADDR 0x40850000u
#define POST_GUARD_END  0x40850040u
SOC_RESERVE_MEMORY_REGION(PRE_GUARD_ADDR, POST_GUARD_END, c5vrx_buzzer_rf_dump_ram);

/* Receiver configuration: the channel is chosen by the caller (5000 + 5 *
 * channel MHz) and changed at run time by rf_set_channel(); bandwidth fixed. */
#define RF_BANDWIDTH        WIFI_BW40

/* MAC TX queue hardware registers (IDF-pinned: ESP32-C5, IDF 6.0.x).
 * Identical to C5VRX-2 wifi5.c proven addresses. */
#define REG32(a)         (*(volatile uint32_t *)(uintptr_t)(a))
#define MAC_TXQ0_CONF    0x600a4d6cu
#define MAC_TXQ_STRIDE   0x10u
#define MAC_TXQ_ENABLE   0x80000000u
#define MAC_TXQ_COUNT    5u

/* Continuous modem front-end un-gating registers.
 * Required to keep the C5 ADC / modem continuously clocking 80 MS/s IQ
 * into MODEM_DIAG when no 802.11 Wi-Fi packets are present. */
#define DUMP_CTRL       0x600a9004u
#define DUMP_PTR_MODE   0x600a9008u
#define DUMP_FORMAT     0x600a9018u
#define FE_PATH         0x600a20b4u
#define FE_ENABLE       0x600a0800u
#define SOURCE_CTRL     0x600a08ccu
#define SOURCE_MUX      0x600a70b8u
#define MODEM_CLOCK     0x600a9c04u
#define CTRL_ENABLE     0x80000000u
#define CTRL_DUMP_FIRST 0x00020000u
#define TX_START_SELECT 0x00060000u
#define SELECTOR_MASK   0x01fe0000u
#define HP_SRAM_USAGE   0x60095004u

/* MODEM_DIAG lane mapping: Q[9:6] on DIAG[6:9], I[9:6] on DIAG[16:19].
 * Lane 2 (Q[8]) and lane 6 (I[7]) use GPIO23/GPIO24 instead of the C5VRX-3
 * GPIO25/GPIO3 so that XIAO pad D2 (GPIO25) and pad A2 (GPIO3) stay free for
 * the buzzer. PARLIO RX (capture.c) reads this table for its data pins. */
static const gpio_num_t s_iq_pins[RF_IQ_LANES] = {
    GPIO_NUM_1, GPIO_NUM_0, GPIO_NUM_23, GPIO_NUM_7,   /* Q[9:6] */
    GPIO_NUM_10, GPIO_NUM_5, GPIO_NUM_24, GPIO_NUM_4,  /* I[9:6] */
};
static const uint8_t s_iq_diag[RF_IQ_LANES] = {
    6u, 7u, 8u, 9u,     /* DIAG[6:9]  = Q[9:6] */
    16u, 17u, 18u, 19u, /* DIAG[16:19] = I[9:6] */
};

/* Internal vendor symbols -- globally exported by the pinned IDF 6.0.x
 * pp (protocol processing) and phy libraries for ESP32-C5. */
extern int lmac_stop_hw_txq(void);
extern void phy_disable_agc(void);
extern void phy_rfagc_disable(void);
extern void phy_wifi_fbw_sel(uint32_t val);
extern void phy_force_rx_gain(bool enable, uint8_t gain_idx);

static const char *TAG = "c5vrx_rf";

static uint8_t  s_channel;
static uint8_t  s_gain = RF_INITIAL_GAIN;
static uint32_t s_retune_us;
static uint32_t s_rearm_count;

const gpio_num_t *rf_iq_lane_gpios(void)
{
    return s_iq_pins;
}

/**
 * Disable all 5 LMAC MAC TX hardware queues.
 * Called once after Wi-Fi start to ensure the frontend is receive-only.
 */
static esp_err_t lock_rx_only(void)
{
    (void)lmac_stop_hw_txq();
    for (unsigned q = 0u; q < MAC_TXQ_COUNT; ++q)
        REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) &= ~MAC_TXQ_ENABLE;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    /* Verify all queues are disabled. */
    for (unsigned q = 0u; q < MAC_TXQ_COUNT; ++q) {
        if ((REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) & MAC_TXQ_ENABLE) != 0u)
            return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/**
 * Route MODEM_DIAG DIAG[6:9] and DIAG[16:19] to the GPIO pins used by
 * PARLIO RX. Called after Wi-Fi initializes the PHY clock domain.
 */
static esp_err_t route_modem_iq(void)
{
    uint64_t mask = 0u;
    for (unsigned lane = 0u; lane < RF_IQ_LANES; ++lane)
        mask |= 1ULL << s_iq_pins[lane];
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
    for (unsigned lane = 0u; lane < RF_IQ_LANES; ++lane) {
        esp_rom_gpio_connect_out_signal(s_iq_pins[lane],
                                        MODEM_DIAG0_IDX + s_iq_diag[lane],
                                        false, false);
    }
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    return ESP_OK;
}

static void rf_enable_continuous_modem(void)
{
    /* Keep CPU ownership of HP SRAM */
    REG32(HP_SRAM_USAGE) = (REG32(HP_SRAM_USAGE) & 0xfffef0ffu) | 0x00010000u;

    /* Un-gate modem clocks and force front-end active. */
    REG32(SOURCE_CTRL) &= 0xff87ffffu;
    REG32(SOURCE_MUX) = (REG32(SOURCE_MUX) & 0xfffffff8u) | 1u;
    REG32(MODEM_CLOCK) = UINT32_MAX;
    REG32(FE_ENABLE) |= 4u;
    REG32(FE_PATH) &= ~1u;

    /* Configure DUMP_FORMAT mode 0 (proven golden RF dump configuration) */
    uint32_t v = REG32(DUMP_FORMAT);
    v = (v & 0xff03ffffu) | 0x006c0000u;
    REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xfffc0fffu) | 0x0001a000u;
    REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xfffff03fu) | 0x00000640u;
    REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xffffffc0u) | 0x18u;
    REG32(DUMP_FORMAT) = v | 0x01000000u;

    /* Set TX_START selector in pre-trigger circular mode (TX_START_SELECT = 0x00060000).
     * Because MAC TX queues are quiescent, TX_START never fires. With CTRL_DUMP_FIRST,
     * the hardware continuously streams pre-trigger samples onto the MODEM_DIAG bus.
     * Crucial: 0x01e00000 software trigger bits are masked out. */
    REG32(DUMP_PTR_MODE) = (REG32(DUMP_PTR_MODE) & ~SELECTOR_MASK) | TX_START_SELECT;

    /* Control: CTRL_DUMP_FIRST, length 16384, ENABLE */
    uint32_t ctrl = REG32(DUMP_CTRL);
    ctrl &= ~(CTRL_ENABLE | 0x00080000u | 0x00040000u); /* Clear ENABLE, START, DONE */
    ctrl |= CTRL_DUMP_FIRST;
    ctrl = (ctrl & ~0x0001ffffu) | 16384u;
    REG32(DUMP_CTRL) = ctrl;

    __asm__ __volatile__("fence iorw, iorw" ::: "memory");

    /* Arm dump engine with ENABLE only */
    REG32(DUMP_CTRL) = ctrl | CTRL_ENABLE;

    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return err;
}

/* Freeze the hardware AGC (Automatic Gain Control). In Wi-Fi mode, the
 * hardware AGC searches for 802.11 preambles; when only analog FM is present,
 * the AGC watchdog periodically steps gain / recalibrates every ~500ms. With
 * the AGC frozen, the captured I/Q amplitude follows the received carrier
 * power for a given forced gain index, which is exactly what the meter needs.
 * Also selects the BW20 analog filter while keeping the 40 MS/s pipeline of
 * BW40. Re-applied after every channel change: the driver's channel switch
 * reprograms parts of the RX chain. */
static void freeze_rx(uint8_t gain_idx)
{
    phy_disable_agc();
    phy_rfagc_disable();
    phy_wifi_fbw_sel(0);
    phy_force_rx_gain(true, gain_idx);
    s_gain = gain_idx;
}

esp_err_t rf_start(uint8_t channel)
{
    /* NVS is required by ESP-IDF Wi-Fi/PHY initialization. */
    esp_err_t err = init_nvs();
    if (err != ESP_OK) return err;

    /* esp_netif_init + default event loop are required by esp_wifi_init().
     * Tolerant of ESP_ERR_INVALID_STATE (already initialized by IDF). */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    /* Initialize Wi-Fi driver with RAM-only storage -- no NVS needed.
     * Crucial: sta_disconnected_pm MUST be false. By default, ESP-IDF enables
     * power management for disconnected stations, periodically shutting down
     * RF, PHY, and BB when idle, which causes periodic loss of MODEM_DIAG clocking. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.sta_disconnected_pm = false;
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;

    /* Force 5 GHz band only. */
#if CONFIG_SOC_WIFI_SUPPORT_5G
    if ((err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY)) != ESP_OK)
        return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif

    /* No power saving -- PHY clock must remain alive at all times. */
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) return err;

    /* Restrict 5 GHz protocols. */
    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                  WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N,
    };
    if ((err = esp_wifi_set_protocols(WIFI_IF_STA, &protocols)) != ESP_OK)
        return err;

    /* Set BW40 on 5 GHz. Hard failure if not available -- NO BW20 fallback.
     * BW40 is a fixed hardware requirement for MODEM_DIAG IQ precision. */
    wifi_bandwidths_t bandwidths = {
        .ghz_2g = WIFI_BW20,
        .ghz_5g = RF_BANDWIDTH,
    };
    err = esp_wifi_set_bandwidths(WIFI_IF_STA, &bandwidths);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BW40 not available (err=%s). No BW20 fallback.", esp_err_to_name(err));
        return err;  /* Hard failure. BW20 produces degraded Q4/I4. */
    }

    /* Wi-Fi channel, e.g. 169 = 5845 MHz, 173 = 5865 MHz. */
    if ((err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE)) != ESP_OK)
        return err;
    s_channel = channel;

    /* Promiscuous mode keeps the RX path and MODEM_DIAG bus active.
     * Zero filter mask prevents LMAC from buffering packets or firing software interrupts. */
    if ((err = esp_wifi_set_promiscuous(true)) != ESP_OK) return err;
    wifi_promiscuous_filter_t filter = { .filter_mask = 0 };
    (void)esp_wifi_set_promiscuous_filter(&filter);

    /* Hardware-disable all 5 LMAC TX queues. Receive-only from here on. */
    if ((err = lock_rx_only()) != ESP_OK) return err;

    /* Verify channel lock. */
    uint8_t primary = 0u;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    if ((err = esp_wifi_get_channel(&primary, &secondary)) != ESP_OK) return err;
    if (primary != channel) {
        ESP_LOGE(TAG, "Channel mismatch: got %u, expected %u", primary, channel);
        return ESP_ERR_INVALID_STATE;
    }

    /* Route MODEM_DIAG to PARLIO RX GPIO pins. */
    if ((err = route_modem_iq()) != ESP_OK) return err;

    /* Un-gate modem ADC clock and force continuous sampling. */
    rf_enable_continuous_modem();

    /* Frozen AGC, BW20 analog filter, C5VRX-3 gain sweet spot; meter.c
     * adjusts the gain from here. */
    freeze_rx(RF_INITIAL_GAIN);

    /* Disable PHY PLL / RXCAL tracking timer if compiled in, so it never
     * recalibrates RF / RX hardware during continuous reception.
     * With CONFIG_ESP_PHY_DISABLE_PLL_TRACK=y, the tracking timer is omitted entirely. */
#if !CONFIG_ESP_PHY_DISABLE_PLL_TRACK
    extern void phy_track_pll_deinit(void);
    phy_track_pll_deinit();
#endif

    ESP_EARLY_LOGW(TAG, "RF ready: %u MHz / ch%u / BW40 / agc=frozen / gain=%u / pll_track=disabled",
                   5000u + 5u * (unsigned)channel, channel, RF_INITIAL_GAIN);
    return ESP_OK;
}

esp_err_t rf_set_channel(uint8_t channel, uint8_t gain_idx)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) return err;
    s_retune_us = (uint32_t)(esp_timer_get_time() - t0);
    s_channel = channel;

    /* The driver's channel switch touches the RX chain: restore receive-only
     * TX queues, the frozen AGC / filter / forced gain, and the dump engine
     * if it disarmed (bit 31 clear means MODEM_DIAG stopped streaming). */
    for (unsigned q = 0u; q < MAC_TXQ_COUNT; ++q)
        REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) &= ~MAC_TXQ_ENABLE;
    freeze_rx(gain_idx);
    if ((REG32(DUMP_CTRL) & CTRL_ENABLE) == 0u) {
        rf_enable_continuous_modem();
        s_rearm_count++;
    }
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    return ESP_OK;
}

uint8_t rf_channel(void)
{
    return s_channel;
}

uint32_t rf_last_retune_us(void)
{
    return s_retune_us;
}

uint32_t rf_dump_rearm_count(void)
{
    return s_rearm_count;
}

void rf_set_rx_gain(bool force, uint8_t gain_idx)
{
    phy_force_rx_gain(force, gain_idx);
    s_gain = gain_idx;
}

uint32_t rf_get_rx_gain_reg(void)
{
    return REG32(0x600a702cu);
}

uint32_t rf_get_dump_ctrl(void)
{
    return REG32(DUMP_CTRL);
}
