#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Cyclic raw Q4/I4 ring filled by PARLIO RX GDMA at 40 MB/s (one byte per
 * 40 MS/s sample, 819 us of signal). Power of two: offsets wrap with a mask. */
#define CAPTURE_RING_BYTES 32768u

/**
 * capture_start() - Start the MODEM_DIAG -> PARLIO RX -> ring transport.
 *
 * PARLIO RX @ 40 MHz, POS sample edge, 8 data lanes from rf_iq_lane_gpios(),
 * infinite soft-delimiter reception into the 32 KiB DMA ring. GDMA RX
 * interrupts are disabled and every descriptor's EOF flag is cleared, as in
 * C5VRX-3, so the hardware streams forever without CPU involvement.
 */
esp_err_t capture_start(void);

/** The ring. Each byte is (I[9:6] << 4) | Q[9:6], two's complement nibbles. */
const uint8_t *capture_ring(void);

/**
 * Byte offset of the DMA block the RX GDMA is currently filling (4 KiB
 * granularity). Data half a ring away from this offset is at least ~400 us
 * old and will not be overwritten for another ~400 us.
 */
uint32_t capture_write_offset(void);

/** Raw PARLIO RX FIFO overflow flag (sticky hardware bit), for telemetry. */
bool capture_rx_fifo_overflow(void);
