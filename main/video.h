#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * video_start() - Initialize and start the PARLIO RX+TX realtime pipeline.
 *
 * Sets up:
 *   - PARLIO RX @ 40 MHz, POS edge, 8-bit, 32 KiB cyclic DMA ring
 *   - PARLIO TX @ 40 MHz, [D,D] output, loop_transmission
 *   - TX BitScrambler with embedded Phase5 LUT (fm.bsasm)
 *   - One-time RX start, then TX starts after a half-ring delay
 *
 * After video_start() returns ESP_OK, the CPU is done.
 * The hardware pipeline runs forever without any software involvement.
 *
 * NO periodic tasks, NO telemetry, NO calibration loading.
 */
esp_err_t video_start(void);

/* ----- Software consumers of the RX ring (experiments/video-link) ----- */

/* Raw ring size (bytes = 40 MS/s samples); video.c asserts it equals
 * RAW_RING_BYTES. */
#define VIDEO_RX_RING_BYTES 32768u

/** The raw RX ring (VIDEO_RX_RING_BYTES, one 40 MS/s Q4/I4 byte per sample). */
const uint8_t *video_rx_ring(void);

/** Ring offset of the descriptor the RX GDMA is writing now: every byte
 * before it (cyclically) is complete. False if the pointer is unknown. */
bool video_rx_write_offset(uint32_t *offset);

/** Largest RX descriptor, bytes (how far the GDMA may be ahead of the
 * write offset). */
uint32_t video_rx_max_descriptor(void);

/** Copy the newest completed RX descriptors that fit in max_bytes into dst,
 * oldest first (time-contiguous). Waits for the next descriptor boundary
 * first, so the oldest block has a full descriptor time before it is
 * overwritten. Returns the bytes copied (0 on failure). */
size_t video_copy_recent_rx(uint8_t *dst, size_t max_bytes);

/** RF gain index in use, and a fixed gain chosen by the caller: the
 * receiver's gain control goes to MANUAL, so the gain only changes when the
 * caller says so. */
uint8_t video_rx_gain(void);
void video_set_rx_gain(uint8_t gain);

#ifdef C5VRX_VIDEO_LINK_EXPERIMENT
/* Implemented by the link component of an experiments/video-link build (its
 * project defines C5VRX_VIDEO_LINK_EXPERIMENT). Such a build has no PARLIO
 * TX, flight BitScrambler or menu: the link replaces the analog output. */

/** Called by video_start() once the RX ring runs and the wide filter is
 * set: take over gain and channel, start the link. */
esp_err_t video_link_start(void);

/** A console key; true when the link handled it. */
bool video_link_console(int key);
#endif
