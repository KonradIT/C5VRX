#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "grab.h"
#include "link_proto.h"

/**
 * link_tx.c - the sending half of the UART video link (protocol:
 * link_proto.h), independent of the UART driver so that the host simulator
 * runs the same code.
 *
 * Per frame: link_tx_frame_begin(), then grab_frame() with link_tx_on_row()
 * and link_tx_on_idle() as its callbacks: captured rows are queued, and
 * encoded into a staging buffer and handed to the port whenever the grabber
 * waits for a line; link_tx_frame_end() encodes what is left, appends the
 * INFO packet and flushes. In LINK_MODE_AUTO the NL delta of the next frame
 * follows the size of this one, so that a frame fits the link in the time
 * the grabber needs for it (two fields).
 */

/** Byte sink: the UART, or the simulator's model of it. */
typedef struct {
    size_t (*tx_free)(void *ctx);                           /* bytes writable without blocking */
    void   (*tx_write)(void *ctx, const uint8_t *data, size_t n);   /* may block when n > tx_free */
    void   *ctx;
} link_tx_port_t;

/** Receiver state reported in every INFO packet. */
typedef struct {
    uint16_t mhz;
    uint8_t  gain;
    uint8_t  channel;       /* index, 0xff unknown */
    uint8_t  scan_locks;
    bool     scanning;
    bool     smoothing;
} link_tx_status_t;

/** Last frame, for the statistics. */
typedef struct {
    uint32_t bytes;         /* on the wire, INFO included */
    int      delta;         /* -1: RAW */
    int      rows_idle;     /* rows encoded while the grabber waited */
    int      rows_after;    /* rows encoded after the grab */
    uint32_t encode_us;     /* all row encoding */
    uint32_t flush_us;      /* time link_tx_frame_end() blocked on the port */
} link_tx_stats_t;

void    link_tx_init(const link_tx_port_t *port, uint32_t baud);
void    link_tx_set_mode(uint8_t mode);     /* LINK_MODE_* */
uint8_t link_tx_mode(void);
int     link_tx_delta(void);                /* delta of the next frame, -1 RAW */

void link_tx_frame_begin(uint8_t frame_id);
void link_tx_on_row(void *ctx, int y, const uint8_t *row);     /* grab_row_cb_t */
bool link_tx_on_idle(void *ctx);                               /* grab_idle_cb_t */
void link_tx_frame_end(int rows, const grab_info_t *info, const link_tx_status_t *st);

/** An INFO packet alone (no picture: no signal, scanning). */
void link_tx_info(uint8_t frame_id, int rows, const grab_info_t *info, const link_tx_status_t *st);

void link_tx_last_stats(link_tx_stats_t *out);

/** INFO payload for a frame (exposed for the tests). */
void link_tx_build_info(uint8_t *out, int rows, const grab_info_t *info, const link_tx_status_t *st,
                        uint8_t mode, int delta);
