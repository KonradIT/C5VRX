#pragma once
/**
 * link_proto.h - C5VRX UART video link, protocol version 2.
 *
 * Shared by the receiver (main/uart_link.c), the display board
 * (tembed/src/main.cpp) and the host tests (test/). Header-only, C and
 * C++, nothing beyond the C library.
 *
 * Packets, both directions:
 *
 *     A5 5A | type | a | b | len | payload[len] | crc16 lo | crc16 hi
 *
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type .. payload. The
 * parser hunts for the magic, waits for the whole packet and checks the CRC;
 * after a bad packet it searches again from the byte after the rejected
 * magic, so a packet hidden inside a corrupted one is still found.
 *
 * Receiver -> display (a = frame counter):
 *   0x11 ROW, b = row 0..167. payload[0] is the row codec:
 *        0 RAW: 224 bytes of 8-bit luma (0 blanking .. 255 nominal white)
 *        1 NL:  near-lossless DPCM, every pixel within +/- delta of the
 *               grabbed value; see link_row_encode()
 *   0x10 INFO, b = 0, LINK_INFO_LEN bytes (layout: LINK_INFO_*). Sent after
 *        the rows of a frame, so it closes the frame; sent alone while there
 *        is no picture (no signal, scanning). Longer payloads are accepted,
 *        the extra bytes ignored.
 * Display -> receiver:
 *   0x20 CMD, a = command (LINK_CMD_*), b = argument, no payload.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LINK_VERSION       2u
#define LINK_W             224
#define LINK_H             168

#define LINK_MAGIC0        0xA5u
#define LINK_MAGIC1        0x5Au
#define LINK_T_INFO        0x10u
#define LINK_T_ROW         0x11u
#define LINK_T_CMD         0x20u

#define LINK_HEADER        6                        /* magic, type, a, b, len */
#define LINK_OVERHEAD      8                        /* header + crc16 */
#define LINK_PAYLOAD_MAX   255
#define LINK_PKT_MAX       (LINK_OVERHEAD + LINK_PAYLOAD_MAX)

/* Row codecs (ROW payload[0]). */
#define LINK_CODEC_RAW     0u
#define LINK_CODEC_NL      1u
#define LINK_ROW_MAX       (1 + LINK_W)             /* longest row payload */
#define LINK_NL_ESC        12                       /* unary cut-off of the Rice code */
#define LINK_NL_DELTA_MAX  15

/* INFO payload. */
#define LINK_INFO_LEN      20
#define LINK_INFO_VERSION  0    /* LINK_VERSION */
#define LINK_INFO_ROWS     1    /* rows captured in this frame (0: no picture) */
#define LINK_INFO_FLAGS    2    /* LINK_F_* */
#define LINK_INFO_MHZ      3    /* 2 bytes, little endian */
#define LINK_INFO_GAIN     5    /* RF gain index */
#define LINK_INFO_ERROR    6    /* LINK_E_* */
#define LINK_INFO_FIELDS   7    /* fields used */
#define LINK_INFO_GRAB_MS  8    /* 2 bytes */
#define LINK_INFO_LINE_NS  10   /* 2 bytes: line period in ns - 60000 */
#define LINK_INFO_CHANNEL  12   /* band * 8 + channel - 1, bands R A B E F L; 0xff unknown */
#define LINK_INFO_LOCKS    13   /* +1 each time a scan locks onto video */
#define LINK_INFO_POWER    14   /* mean I^2 + Q^2 of the samples (4-bit units) */
#define LINK_INFO_CLIP     15   /* % of samples at full scale */
#define LINK_INFO_DELTA    16   /* NL delta of this frame's rows, 0xff raw */
#define LINK_INFO_JITTER   17   /* rms sync-edge error, 4 ns units */
#define LINK_INFO_LATE     18   /* rows lost to CPU lateness, saturating */
#define LINK_INFO_NOSYNC   19   /* rows without a sync edge, saturating */

#define LINK_F_PAL         0x01u
#define LINK_F_VIDEO       0x02u
#define LINK_F_MODE_SHIFT  2    /* 2 bits: LINK_MODE_* */
#define LINK_F_SCANNING    0x10u
#define LINK_F_SMOOTHING   0x20u

#define LINK_E_NONE        0u
#define LINK_E_NO_HSYNC    1u
#define LINK_E_NO_VSYNC    2u   /* no vertical interval found */
#define LINK_E_LOST        3u   /* lost horizontal sync */
#define LINK_E_TIMEOUT     4u
#define LINK_E_OTHER       5u
#define LINK_E_LOST_VSYNC  6u

/* Picture modes: how the receiver trades quality against frame rate. */
#define LINK_MODE_AUTO     0u   /* NL, delta chosen per frame to fit the link at full frame rate */
#define LINK_MODE_FINE     1u   /* NL, delta 1 (visually lossless), lower frame rate */
#define LINK_MODE_RAW      2u   /* 8-bit rows, about 10 fps at 4 Mbaud */
#define LINK_MODE_COUNT    3u

/* Commands (CMD a). */
#define LINK_CMD_SCAN_START   1u
#define LINK_CMD_SCAN_STOP    2u
#define LINK_CMD_CHANNEL_STEP 3u   /* b: int8 steps, skipping untunable channels */
#define LINK_CMD_MODE         4u   /* b: LINK_MODE_* */
#define LINK_CMD_SMOOTHING    5u   /* b: 0 off, 1 on */
#define LINK_CMD_CHANNEL_SET  6u   /* b: channel index */

/* ------------------------------------------------------------------ CRC */

static const uint16_t k_link_crc16[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0,
};

static inline uint16_t link_crc16(const uint8_t *p, int n)
{
    uint16_t c = 0xFFFFu;
    while (n-- > 0) c = (uint16_t)((c << 8) ^ k_link_crc16[(uint8_t)((c >> 8) ^ *p++)]);
    return c;
}

/* ------------------------------------------------------------------ packets */

/* Complete a packet whose payload is already at pkt + LINK_HEADER. Returns
 * the packet size. */
static inline int link_packet_seal(uint8_t *pkt, uint8_t type, uint8_t a, uint8_t b, int len)
{
    pkt[0] = LINK_MAGIC0;
    pkt[1] = LINK_MAGIC1;
    pkt[2] = type;
    pkt[3] = a;
    pkt[4] = b;
    pkt[5] = (uint8_t)len;
    uint16_t c = link_crc16(pkt + 2, 4 + len);
    pkt[LINK_HEADER + len] = (uint8_t)(c & 0xffu);
    pkt[LINK_HEADER + len + 1] = (uint8_t)(c >> 8);
    return LINK_OVERHEAD + len;
}

/* Build a packet in out (at least len + LINK_OVERHEAD bytes). */
static inline int link_packet(uint8_t *out, uint8_t type, uint8_t a, uint8_t b, const uint8_t *payload, int len)
{
    if (len > 0) memmove(out + LINK_HEADER, payload, (size_t)len);
    return link_packet_seal(out, type, a, b, len);
}

typedef void (*link_packet_cb_t)(void *ctx, uint8_t type, uint8_t a, uint8_t b, const uint8_t *payload, int len);

typedef struct {
    uint8_t  q[2 * LINK_PKT_MAX];   /* unparsed bytes */
    int      n;
    uint32_t packets;               /* good packets */
    uint32_t crc_errors;            /* complete candidates with a bad CRC */
    uint32_t skipped;               /* bytes skipped while hunting for a magic */
} link_parser_t;

static inline void link_parser_init(link_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

/* Feed received bytes; cb runs for every packet with a good CRC. */
static inline void link_parser_feed(link_parser_t *p, const uint8_t *data, int n, link_packet_cb_t cb, void *ctx)
{
    while (n > 0) {
        int take = (int)sizeof(p->q) - p->n;
        if (take > n) take = n;
        memcpy(p->q + p->n, data, (size_t)take);
        p->n += take;
        data += take;
        n -= take;

        int s = 0;
        for (;;) {
            while (s < p->n && !(p->q[s] == LINK_MAGIC0 && (s + 1 == p->n || p->q[s + 1] == LINK_MAGIC1))) {
                s++;
                p->skipped++;
            }
            if (s + LINK_HEADER > p->n) break;                      /* header incomplete */
            int len = p->q[s + 5];
            int size = LINK_OVERHEAD + len;
            if (s + size > p->n) break;                             /* packet incomplete */
            const uint8_t *k = p->q + s;
            uint16_t c = link_crc16(k + 2, 4 + len);
            if ((uint8_t)(c & 0xffu) == k[LINK_HEADER + len] && (uint8_t)(c >> 8) == k[LINK_HEADER + len + 1]) {
                p->packets++;
                if (cb) cb(ctx, k[2], k[3], k[4], k + LINK_HEADER, len);
                s += size;
            } else {
                p->crc_errors++;
                s += 1;                                             /* look inside it for the next magic */
            }
        }
        if (s > 0) {
            memmove(p->q, p->q + s, (size_t)(p->n - s));
            p->n -= s;
        }
    }
}

/* ------------------------------------------------------------------ rows */

/* NL row codec. Pixel 0 is sent as is; every later pixel x is predicted by
 * the reconstructed pixel x - 1, the prediction error quantized with step
 * 2 delta + 1 (so the reconstruction is within +/- delta of the input;
 * delta 0 is lossless) and the quantized error qe mapped to u = 2 qe
 * (qe >= 0) or -2 qe - 1 and written as a Rice code with parameter k: the
 * quotient u >> k in unary (that many 0 bits, then a 1), then the k low bits
 * of u; a quotient of LINK_NL_ESC or more is written as LINK_NL_ESC 0 bits
 * and u in 9 bits. The encoder picks k from the rows before.
 *
 * Payload: [0] LINK_CODEC_NL, [1] delta | k << 4, [2] pixel 0, then the
 * codes, most significant bit first, zero-padded to a byte. A row the code
 * would make longer than RAW is sent RAW. */

typedef struct {
    int16_t  step;          /* reconstruction step qe (2 delta + 1) */
    uint16_t code;          /* u = 2 qe or -2 qe - 1 */
} link_nl_entry_t;

typedef struct {
    int      delta;         /* -1: RAW */
    int      k;             /* Rice parameter of the next row (from the rows before) */
    link_nl_entry_t t[511]; /* by prediction error e at [e + 255] */
    uint8_t  clamp[320];    /* reconstruction r at [r + 32], clamped to 0..255 */
} link_encoder_t;

static inline void link_encoder_set_delta(link_encoder_t *e, int delta)
{
    if (delta > LINK_NL_DELTA_MAX) delta = LINK_NL_DELTA_MAX;
    e->delta = delta;
    if (e->k < 0 || e->k > 7) e->k = 2;
    if (delta < 0) return;
    const int q = 2 * delta + 1;
    for (int d = -255; d <= 255; ++d) {
        int qe = d >= 0 ? (d + delta) / q : -((-d + delta) / q);
        e->t[d + 255].step = (int16_t)(qe * q);
        e->t[d + 255].code = (uint16_t)(qe >= 0 ? 2 * qe : -2 * qe - 1);
    }
    for (int r = -32; r < 288; ++r) e->clamp[r + 32] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
}

/* Rice parameter for n codes summing to sum: the smallest k with
 * n 2^k >= 0.6 sum (within 0.3 % of the best k on real frames; JPEG-LS uses
 * 1.0, 6 % worse here). */
static inline int link_rice_k(uint32_t n, uint32_t sum)
{
    int k = 0;
    while (k < 7 && ((5u * n) << k) < 3u * sum) k++;
    return k;
}

/* Encode one LINK_W-pixel row into out (LINK_ROW_MAX bytes); returns the
 * payload length. rec, if not NULL, receives what the display will show.
 * Runs inside the receiver's grab loop, so it is a single pass with table
 * lookups: the Rice parameter comes from the previous row (1.5 % larger
 * than the best one per row on real frames). */
static inline int link_row_encode(link_encoder_t *e, const uint8_t *px, uint8_t *out, uint8_t *rec)
{
    if (e->delta >= 0) {
        const int k = e->k;
        const uint32_t one = 1u << k, mask = one - 1u;
        out[0] = LINK_CODEC_NL;
        out[1] = (uint8_t)(e->delta | (k << 4));
        out[2] = px[0];
        uint8_t *p = out + 3;
        uint8_t *const limit = out + LINK_ROW_MAX - 4;     /* room for one more code (<= 21 bits) */
        uint32_t acc = 0u, sum = 0u;
        int n = 0, r = px[0], x = 1;
        if (rec) rec[0] = px[0];
        for (; x < LINK_W && p < limit; ++x) {
            const link_nl_entry_t t = e->t[px[x] - r + 255];
            r = e->clamp[r + t.step + 32];
            if (rec) rec[x] = (uint8_t)r;
            const uint32_t v = t.code;
            sum += v;
            const uint32_t h = v >> k;
            if (h < (uint32_t)LINK_NL_ESC) {
                const int bits = (int)h + 1 + k;
                acc = (acc << bits) | one | (v & mask);
                n += bits;
            } else {
                acc = (acc << (LINK_NL_ESC + 9)) | v;      /* LINK_NL_ESC zero bits, then u in 9 bits */
                n += LINK_NL_ESC + 9;
            }
            while (n >= 8) {
                n -= 8;
                *p++ = (uint8_t)(acc >> n);
            }
        }
        e->k = link_rice_k((uint32_t)(x - 1), sum);
        if (x == LINK_W) {
            if (n > 0) *p++ = (uint8_t)(acc << (8 - n));
            return (int)(p - out);
        }
    }
    out[0] = LINK_CODEC_RAW;
    memcpy(out + 1, px, LINK_W);
    if (rec) memcpy(rec, px, LINK_W);
    return 1 + LINK_W;
}

/* Decode a row payload into px (LINK_W pixels). False for a malformed row
 * (px may then be partly written). */
static inline bool link_row_decode(const uint8_t *in, int len, uint8_t *px)
{
    if (len < 1) return false;
    if (in[0] == LINK_CODEC_RAW) {
        if (len < 1 + LINK_W) return false;
        memcpy(px, in + 1, LINK_W);
        return true;
    }
    if (in[0] != LINK_CODEC_NL || len < 3) return false;
    const int delta = in[1] & 15, k = (in[1] >> 4) & 7, q = 2 * delta + 1;
    const uint8_t *p = in + 3, *end = in + len;
    uint32_t acc = 0u;          /* next bits, left aligned */
    int n = 0;                  /* valid bits in acc */
    int r = in[2];
    px[0] = (uint8_t)r;
    for (int x = 1; x < LINK_W; ++x) {
        while (n <= 24 && p < end) {
            acc |= (uint32_t)*p++ << (24 - n);
            n += 8;
        }
        int z = acc ? __builtin_clz(acc) : 32;
        unsigned v;
        if (z < LINK_NL_ESC) {
            int need = z + 1 + k;
            if (need > n) return false;                 /* out of data */
            v = ((unsigned)z << k) | (k ? (acc << (z + 1)) >> (32 - k) : 0u);
            acc = need < 32 ? acc << need : 0u;
            n -= need;
        } else {
            int need = LINK_NL_ESC + 9;
            if (need > n) return false;
            v = (acc << LINK_NL_ESC) >> (32 - 9);
            acc <<= need;
            n -= need;
        }
        int qe = (v & 1u) ? -(int)((v + 1u) >> 1) : (int)(v >> 1);
        r += qe * q;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        px[x] = (uint8_t)r;
    }
    return true;
}

/* ------------------------------------------------------------------ channels */

/* "R6" etc. for a channel index (band * 8 + channel - 1, bands R A B E F L). */
static inline void link_channel_name(uint8_t index, char name[3])
{
    static const char k_bands[] = "RABEFL";
    if (index >= 48u) {
        name[0] = '-';
        name[1] = '-';
    } else {
        name[0] = k_bands[index / 8u];
        name[1] = (char)('1' + index % 8u);
    }
    name[2] = '\0';
}
