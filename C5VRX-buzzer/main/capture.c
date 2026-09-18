/**
 * capture.c - MODEM_DIAG Q4/I4 @ 40 MS/s -> PARLIO RX -> 32 KiB cyclic GDMA ring.
 *
 * This is the receive half of C5VRX-3 video.c, unchanged except for the lane
 * GPIO table (taken from rf.c) and the removal of everything downstream of
 * the ring: no BitScrambler, no PARLIO TX, no DAC. The CPU reads the ring at
 * its leisure (meter.c); the hardware never waits for it.
 */

#include "capture.h"
#include "rf.h"

#include <stdint.h>
#include <string.h>

#include "driver/parlio_rx.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "hal/dma_types.h"
#include "soc/ahb_dma_struct.h"
#include "soc/parl_io_struct.h"

#define IQ_RATE_HZ 40000000u   /* MODEM_DIAG / PARLIO RX clock */

static const char *TAG = "c5vrx_capture";

/* DMA-aligned ring buffer in HP SRAM, written by the RX GDMA at 40 MB/s. */
static DMA_ATTR __attribute__((aligned(64))) uint8_t s_raw_ring[CAPTURE_RING_BYTES];

static parlio_rx_unit_handle_t      s_rx;
static parlio_rx_delimiter_handle_t s_rx_delimiter;
static int                          s_rx_dma_ch = -1;

const uint8_t *capture_ring(void)
{
    return s_raw_ring;
}

static esp_err_t prepare_rx(void)
{
    /* Proven PARLIO RX config with clean internal SPLL clock (C5VRX-3). */
    parlio_rx_unit_config_t cfg = {
        .trans_queue_depth = 1u,
        .max_recv_size     = sizeof(s_raw_ring),
        .dma_burst_size    = 32u,
        .data_width        = RF_IQ_LANES,
        .clk_src           = PARLIO_CLK_SRC_DEFAULT,
        .ext_clk_freq_hz   = 0u,
        .exp_clk_freq_hz   = IQ_RATE_HZ,
        .clk_in_gpio_num   = -1,
        .clk_out_gpio_num  = -1,
        .valid_gpio_num    = -1,
        .flags = {
            .free_clk    = true,   /* RX clock is derived from PHY, not gated */
            .clk_gate_en = false,
            .allow_pd    = false,
        },
    };
    /* Same GPIOs, same order as the MODEM_DIAG lane routing in rf.c. */
    const gpio_num_t *lanes = rf_iq_lane_gpios();
    for (unsigned lane = 0u; lane < RF_IQ_LANES; ++lane)
        cfg.data_gpio_nums[lane] = lanes[lane];

    esp_err_t err = parlio_new_rx_unit(&cfg, &s_rx);
    if (err != ESP_OK) return err;

    /* Soft delimiter in infinite (partial_rx_en) mode, POS sample edge. */
    const parlio_rx_soft_delimiter_config_t delim_cfg = {
        .sample_edge    = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len   = sizeof(s_raw_ring),
        .timeout_ticks  = 0u,
    };
    err = parlio_new_rx_soft_delimiter(&delim_cfg, &s_rx_delimiter);
    if (err != ESP_OK) return err;

    return parlio_rx_unit_enable(s_rx, false);
}

static esp_err_t start_rx(void)
{
    esp_err_t err = parlio_rx_soft_delimiter_start_stop(s_rx, s_rx_delimiter, true);
    if (err != ESP_OK) return err;
    const parlio_receive_config_t cfg = {
        .delimiter = s_rx_delimiter,
        .flags = {
            .partial_rx_en  = true,
            .indirect_mount = false,
        },
    };
    return parlio_rx_unit_receive(s_rx, s_raw_ring, sizeof(s_raw_ring), &cfg);
}

uint32_t capture_write_offset(void)
{
    if (s_rx_dma_ch < 0 || s_rx_dma_ch >= 3) return 0u;
    uint32_t dscr_addr = AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val;
    if (dscr_addr >= 0x40800000u && dscr_addr < 0x40860000u) {
        const dma_descriptor_t *dscr = (const dma_descriptor_t *)(uintptr_t)dscr_addr;
        const uint8_t *buf = (const uint8_t *)dscr->buffer;
        if (buf >= s_raw_ring && buf < s_raw_ring + sizeof(s_raw_ring))
            return (uint32_t)(buf - s_raw_ring);
    }
    return 0u;
}

/* Clear suc_eof on every descriptor of the cyclic RX chain so the ring never
 * raises a wrap EOF (C5VRX-3 "Zero-EOF" patch). */
static int patch_descriptors_clear_eof(int dma_ch)
{
    if (dma_ch < 0 || dma_ch >= 3) return 0;
    uint32_t first_addr = AHB_DMA.channel[dma_ch].in.in_dscr_bf0.val;
    if (first_addr < 0x40800000u || first_addr >= 0x40860000u) return 0;

    dma_descriptor_t *curr = (dma_descriptor_t *)(uintptr_t)first_addr;
    int count = 0;
    while (curr && count < 64) {
        curr->dw0.suc_eof = 0;
        (void)esp_cache_msync(curr, sizeof(dma_descriptor_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        curr = curr->next;
        count++;
        if ((uintptr_t)curr == first_addr) break;
    }
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    return count;
}

bool capture_rx_fifo_overflow(void)
{
    return PARL_IO.int_raw.rx_fifo_wovf_int_raw != 0;
}

esp_err_t capture_start(void)
{
    memset(s_raw_ring, 0, sizeof(s_raw_ring));
    (void)esp_cache_msync(s_raw_ring, sizeof(s_raw_ring), ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    esp_err_t err;
    if ((err = prepare_rx()) != ESP_OK) return err;
    if ((err = start_rx()) != ESP_OK) return err;

    /* Pure continuous hardware mode: no GDMA RX interrupts, no RX EOF events. */
    AHB_DMA.in_intr[0].ena.val = 0;
    AHB_DMA.in_intr[1].ena.val = 0;
    AHB_DMA.in_intr[2].ena.val = 0;
    PARL_IO.rx_genrl_cfg.rx_eof_gen_sel = 1;

    /* Discover the AHB_DMA channel assigned to PARL_IO RX (peripheral ID 9). */
    for (int i = 0; i < 3; i++) {
        if (AHB_DMA.channel[i].in.in_peri_sel.peri_in_sel_chn == 9)
            s_rx_dma_ch = i;
    }
    int rx_nodes = patch_descriptors_clear_eof(s_rx_dma_ch);

    ESP_EARLY_LOGW(TAG, "capture ready: 40 MS/s Q4/I4, %u byte ring, rx_ch=%d, %d descriptors patched",
                   (unsigned)sizeof(s_raw_ring), s_rx_dma_ch, rx_nodes);
    return ESP_OK;
}
