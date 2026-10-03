/**
 * link_test.c - host tests for protocol/link_proto.h (UART link protocol v2).
 *
 *  1. CRC-16/CCITT-FALSE check value.
 *  2. Row codec: every delta on synthetic rows (flat, ramps, noise, full
 *     swing, single spikes) and on real frames: the decoder reproduces the
 *     encoder's reconstruction exactly and stays within +/- delta.
 *  3. Parser: a stream of frames with bit errors, dropped and inserted
 *     bytes; every packet that arrives intact is delivered exactly once,
 *     nothing corrupted is.
 *  4. Compression on real frames (PGM, 224x168): bytes per row, PSNR and
 *     the frame rate the link would allow at 4 and 5 Mbaud.
 *
 * build: test/run_tests.sh
 * usage: link_test [frame.pgm ...]
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "link_proto.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint32_t g_rng = 12345u;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

/* ------------------------------------------------------------ codec */

static void roundtrip(const uint8_t *px, int delta, long *bytes, double *se)
{
    static link_encoder_t enc;
    uint8_t out[LINK_ROW_MAX], rec[LINK_W], dec[LINK_W];
    link_encoder_set_delta(&enc, delta);
    int len = link_row_encode(&enc, px, out, rec);
    CHECK(len >= 3 && len <= LINK_ROW_MAX, "delta %d: length %d", delta, len);
    bool ok = link_row_decode(out, len, dec);
    CHECK(ok, "delta %d: decode failed", delta);
    CHECK(memcmp(dec, rec, LINK_W) == 0, "delta %d: decoder differs from the encoder's reconstruction", delta);
    int lim = delta < 0 ? 0 : delta;
    for (int x = 0; x < LINK_W; ++x) {
        int d = (int)px[x] - (int)dec[x];
        if (abs(d) > lim) { CHECK(0, "delta %d: pixel %d error %d", delta, x, d); break; }
        if (se) *se += (double)d * d;
    }
    /* Truncated rows must be rejected, not read past their end. */
    if (out[0] == LINK_CODEC_NL && len > 4) {
        uint8_t tmp[LINK_W];
        CHECK(!link_row_decode(out, 3, tmp), "delta %d: 3-byte row accepted", delta);
    }
    if (bytes) *bytes += len;
}

static void test_codec_synthetic(void)
{
    uint8_t px[LINK_W];
    for (int delta = -1; delta <= LINK_NL_DELTA_MAX; ++delta) {
        for (int pattern = 0; pattern < 9; ++pattern) {
            for (int x = 0; x < LINK_W; ++x) {
                switch (pattern) {
                case 0: px[x] = 0; break;
                case 1: px[x] = 255; break;
                case 2: px[x] = (uint8_t)x; break;
                case 3: px[x] = (uint8_t)(255 - x); break;
                case 4: px[x] = (x & 1) ? 255 : 0; break;                           /* worst case */
                case 5: px[x] = (uint8_t)(rnd() & 255); break;
                case 6: px[x] = (uint8_t)(128 + (int)(rnd() % 9) - 4); break;          /* flat + noise */
                case 7: px[x] = (x % 37 == 0) ? 255 : 20; break;                        /* spikes */
                default: px[x] = (uint8_t)(128 + 100 * sin(x * 0.2)); break;
                }
            }
            roundtrip(px, delta, NULL, NULL);
        }
    }
    /* Malformed payloads. */
    uint8_t bad[LINK_ROW_MAX], dec[LINK_W];
    memset(bad, 0, sizeof(bad));
    bad[0] = LINK_CODEC_NL;
    CHECK(!link_row_decode(bad, 40, dec), "all-zero NL row accepted");   /* escapes run out of data */
    bad[0] = 7;
    CHECK(!link_row_decode(bad, 100, dec), "unknown codec accepted");
    bad[0] = LINK_CODEC_RAW;
    CHECK(!link_row_decode(bad, 100, dec), "short RAW row accepted");
    printf("codec: synthetic rows ok\n");
}

/* ------------------------------------------------------------ parser */

typedef struct {
    int     n;
    uint8_t type[4096], a[4096], b[4096];
    int     len[4096];
    uint16_t sum[4096];
} rx_log_t;

static void on_packet(void *ctx, uint8_t type, uint8_t a, uint8_t b, const uint8_t *payload, int len)
{
    rx_log_t *log = (rx_log_t *)ctx;
    if (log->n >= 4096) return;
    uint16_t s = 0;
    for (int i = 0; i < len; ++i) s = (uint16_t)(s * 31u + payload[i]);
    log->type[log->n] = type;
    log->a[log->n] = a;
    log->b[log->n] = b;
    log->len[log->n] = len;
    log->sum[log->n] = s;
    log->n++;
}

static void test_parser(void)
{
    static uint8_t stream[1 << 20];
    static int pkt_start[4096], pkt_size[4096];
    static uint8_t damaged[1 << 20];
    static rx_log_t sent, got;
    int n = 0, np = 0;
    static link_encoder_t enc;
    link_encoder_set_delta(&enc, 2);
    uint8_t px[LINK_W];
    for (int frame = 0; frame < 12; ++frame) {
        for (int row = 0; row < LINK_H; ++row) {
            for (int x = 0; x < LINK_W; ++x) px[x] = (uint8_t)(x + row + frame * 7 + (rnd() & 7));
            uint8_t *pkt = stream + n;
            int len = link_row_encode(&enc, px, pkt + LINK_HEADER, NULL);
            int size = link_packet_seal(pkt, LINK_T_ROW, (uint8_t)frame, (uint8_t)row, len);
            pkt_start[np] = n;
            pkt_size[np] = size;
            on_packet(&sent, LINK_T_ROW, (uint8_t)frame, (uint8_t)row, pkt + LINK_HEADER, len);
            np++;
            n += size;
        }
        uint8_t info[LINK_INFO_LEN];
        for (int i = 0; i < LINK_INFO_LEN; ++i) info[i] = (uint8_t)(rnd() & 255);
        info[0] = (uint8_t)LINK_VERSION;
        pkt_start[np] = n;
        pkt_size[np] = link_packet(stream + n, LINK_T_INFO, (uint8_t)frame, 0, info, LINK_INFO_LEN);
        on_packet(&sent, LINK_T_INFO, (uint8_t)frame, 0, info, LINK_INFO_LEN);
        n += pkt_size[np++];
    }

    /* Clean stream, fed in random chunk sizes. */
    link_parser_t p;
    link_parser_init(&p);
    for (int i = 0; i < n; ) {
        int c = 1 + (int)(rnd() % 700);
        if (c > n - i) c = n - i;
        link_parser_feed(&p, stream + i, c, on_packet, &got);
        i += c;
    }
    CHECK(got.n == sent.n, "clean stream: %d of %d packets", got.n, sent.n);
    CHECK(p.crc_errors == 0 && p.skipped == 0, "clean stream: %u crc errors, %u bytes skipped",
          (unsigned)p.crc_errors, (unsigned)p.skipped);
    for (int i = 0; i < got.n && i < sent.n; ++i)
        if (got.sum[i] != sent.sum[i] || got.b[i] != sent.b[i]) { CHECK(0, "clean stream: packet %d differs", i); break; }

    /* Damage: bit flips, dropped bytes, inserted garbage (with fake magics). */
    int m = 0;
    static bool intact[4096];
    int next_pkt = 0;
    int flips = 0, drops = 0, inserts = 0;
    for (int k = 0; k < np; ++k) intact[k] = true;
    for (int i = 0; i < n; ++i) {
        while (next_pkt + 1 < np && pkt_start[next_pkt + 1] <= i) next_pkt++;
        uint32_t r = rnd() % 20000u;
        if (r < 3) { drops++; intact[next_pkt] = false; continue; }
        uint8_t b = stream[i];
        if (r >= 3 && r < 8) { b ^= (uint8_t)(1u << (rnd() % 8)); flips++; intact[next_pkt] = false; }
        damaged[m++] = b;
        if (r >= 8 && r < 10) {
            /* garbage between packets or inside one */
            int g = 1 + (int)(rnd() % 12);
            for (int j = 0; j < g; ++j) damaged[m++] = (j & 1) ? LINK_MAGIC1 : LINK_MAGIC0;
            inserts++;
            if (i + 1 < pkt_start[next_pkt] + pkt_size[next_pkt]) intact[next_pkt] = false;
        }
    }
    static rx_log_t got2;
    link_parser_t p2;
    link_parser_init(&p2);
    for (int i = 0; i < m; ) {
        int c = 1 + (int)(rnd() % 300);
        if (c > m - i) c = m - i;
        link_parser_feed(&p2, damaged + i, c, on_packet, &got2);
        i += c;
    }
    /* Every delivered packet must be one that was sent, in order, and every
     * intact one must be delivered. */
    int si = 0, delivered_intact = 0, n_intact = 0, bogus = 0;
    for (int k = 0; k < np; ++k) n_intact += intact[k];
    for (int g = 0; g < got2.n; ++g) {
        while (si < sent.n && !(sent.type[si] == got2.type[g] && sent.a[si] == got2.a[g] &&
                                sent.b[si] == got2.b[g] && sent.sum[si] == got2.sum[g]))
            si++;
        if (si == sent.n) { bogus++; si = 0; continue; }
        if (intact[si]) delivered_intact++;
        si++;
    }
    CHECK(bogus == 0, "damaged stream: %d packets delivered that were never sent", bogus);
    CHECK(delivered_intact == n_intact, "damaged stream: %d of %d intact packets delivered", delivered_intact, n_intact);
    printf("parser: %d packets, %d flips, %d drops, %d inserts: %d intact, %d delivered, %u crc errors\n",
           np, flips, drops, inserts, n_intact, got2.n, (unsigned)p2.crc_errors);
}

/* ------------------------------------------------------------ real frames */

static int load_pgm(const char *fn, uint8_t *img)
{
    FILE *f = fopen(fn, "rb");
    if (!f) return 0;
    char magic[3] = { 0 };
    int w, h, mx;
    int ok = fscanf(f, "%2s %d %d %d", magic, &w, &h, &mx) == 4 && w == LINK_W && h == LINK_H;
    if (ok) {
        fgetc(f);
        ok = fread(img, 1, LINK_W * LINK_H, f) == (size_t)(LINK_W * LINK_H);
    }
    fclose(f);
    return ok;
}

static void test_frames(int argc, char **argv)
{
    static uint8_t img[LINK_H * LINK_W];
    if (argc < 2) return;
    printf("%-6s %8s %8s %8s %10s %10s\n", "delta", "B/row", "bits/px", "PSNR", "fps 4M", "fps 5M");
    for (int delta = -1; delta <= 8; ++delta) {
        long bytes = 0;
        double se = 0.0;
        int frames = 0;
        for (int a = 1; a < argc; ++a) {
            if (!load_pgm(argv[a], img)) { printf("cannot read %s\n", argv[a]); g_fail++; return; }
            for (int y = 0; y < LINK_H; ++y) roundtrip(img + y * LINK_W, delta, &bytes, &se);
            frames++;
        }
        double brow = (double)bytes / (frames * LINK_H);
        double fbytes = LINK_H * (LINK_OVERHEAD + brow) + LINK_OVERHEAD + LINK_INFO_LEN;
        double mse = se / ((double)frames * LINK_W * LINK_H);
        char name[8];
        snprintf(name, sizeof(name), delta < 0 ? "raw" : "%d", delta);
        printf("%-6s %8.1f %8.2f %8.2f %10.1f %10.1f\n", name, brow, (brow - 3) * 8 / (LINK_W - 1),
               mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99.0, 400000.0 / fbytes, 500000.0 / fbytes);
    }
}

int main(int argc, char **argv)
{
    uint16_t c = link_crc16((const uint8_t *)"123456789", 9);
    CHECK(c == 0x29B1, "crc16 check value %04x", c);
    test_codec_synthetic();
    test_parser();
    test_frames(argc, argv);
    printf(g_fail ? "link_test: %d FAILURES\n" : "link_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
