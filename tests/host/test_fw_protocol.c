/**
 * Host unit tests for the fw_protocol component (runs on a PC, no ESP32).
 * Reference frames are shared with tools/tests/test_fwp.py, which proves the
 * C firmware and the Python flasher speak exactly the same protocol.
 *
 *   make -C tests/host test
 */
#include <stdio.h>
#include <string.h>

#include "fw_protocol.h"

static int s_failures;
static int s_checks;

#define CHECK(cond)                                                       \
    do {                                                                  \
        s_checks++;                                                       \
        if (!(cond)) {                                                    \
            s_failures++;                                                 \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                 \
    } while (0)

static const uint8_t CHECK_STR[] = "123456789";

/* Same bytes as REF_PING / REF_START in tools/tests/test_fwp.py */
static const uint8_t REF_PING[]  = { 0xA5, 0x01, 0x00, 0x00, 0xAC, 0xFB };
static const uint8_t REF_START[] = { 0xA5, 0x02, 0x08, 0x00, 0x00, 0x10, 0x00, 0x00,
                                     0x26, 0x39, 0xF4, 0xCB, 0x4D, 0xF5 };

static fwp_parse_result_t feed_all(fwp_parser_t *p, const uint8_t *data, size_t len)
{
    fwp_parse_result_t last = FWP_PARSE_INCOMPLETE;
    for (size_t i = 0; i < len; i++) {
        fwp_parse_result_t r = fwp_parser_feed(p, data[i]);
        if (r != FWP_PARSE_INCOMPLETE) {
            last = r;
        }
    }
    return last;
}

static void test_crc_reference_values(void)
{
    CHECK(fwp_crc16_update(0xFFFF, CHECK_STR, 9) == 0x29B1); /* CRC-16/CCITT-FALSE */
    CHECK(fwp_crc32_update(0, CHECK_STR, 9) == 0xCBF43926u);  /* CRC-32 (zlib) */
}

static void test_crc32_is_chainable(void)
{
    uint32_t crc = fwp_crc32_update(0, CHECK_STR, 4);
    crc = fwp_crc32_update(crc, CHECK_STR + 4, 5);
    CHECK(crc == 0xCBF43926u);
}

static void test_encode_matches_reference(void)
{
    uint8_t out[FWP_MAX_FRAME];
    uint8_t start[8];

    CHECK(fwp_encode(FWP_CMD_PING, NULL, 0, out, sizeof(out)) == sizeof(REF_PING));
    CHECK(memcmp(out, REF_PING, sizeof(REF_PING)) == 0);

    fwp_put_u32_le(&start[0], 0x1000);
    fwp_put_u32_le(&start[4], 0xCBF43926u);
    CHECK(fwp_encode(FWP_CMD_START, start, 8, out, sizeof(out)) == sizeof(REF_START));
    CHECK(memcmp(out, REF_START, sizeof(REF_START)) == 0);
}

static void test_encode_rejects_bad_sizes(void)
{
    uint8_t out[16];
    uint8_t payload[FWP_MAX_PAYLOAD + 1] = { 0 };

    CHECK(fwp_encode(FWP_CMD_DATA, payload, 32, out, sizeof(out)) == 0);           /* out too small */
    CHECK(fwp_encode(FWP_CMD_DATA, payload, FWP_MAX_PAYLOAD + 1, out, 4096) == 0); /* too long */
}

static void test_parse_roundtrip(void)
{
    fwp_parser_t p;
    uint8_t      payload[FWP_MAX_PAYLOAD];
    uint8_t      frame[FWP_MAX_FRAME];

    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7);
    }
    size_t n = fwp_encode(FWP_CMD_DATA, payload, sizeof(payload), frame, sizeof(frame));

    fwp_parser_init(&p);
    CHECK(feed_all(&p, frame, n) == FWP_PARSE_FRAME);
    CHECK(p.frame.cmd == FWP_CMD_DATA);
    CHECK(p.frame.len == sizeof(payload));
    CHECK(memcmp(p.frame.payload, payload, sizeof(payload)) == 0);
}

static void test_parse_skips_noise_before_frame(void)
{
    fwp_parser_t  p;
    const uint8_t noise[] = "I (1234) main: log text on the same UART\r\n";

    fwp_parser_init(&p);
    CHECK(feed_all(&p, noise, sizeof(noise) - 1) == FWP_PARSE_INCOMPLETE);
    CHECK(feed_all(&p, REF_PING, sizeof(REF_PING)) == FWP_PARSE_FRAME);
    CHECK(p.frame.cmd == FWP_CMD_PING && p.frame.len == 0);
}

static void test_parse_detects_corruption(void)
{
    fwp_parser_t p;
    uint8_t      bad[sizeof(REF_START)];

    memcpy(bad, REF_START, sizeof(bad));
    bad[6] ^= 0x01; /* one bit flipped in the payload */

    fwp_parser_init(&p);
    CHECK(feed_all(&p, bad, sizeof(bad)) == FWP_PARSE_CRC_ERROR);
    /* Parser recovers for the next, valid frame */
    CHECK(feed_all(&p, REF_START, sizeof(REF_START)) == FWP_PARSE_FRAME);
}

static void test_parse_rejects_oversized_length(void)
{
    fwp_parser_t  p;
    const uint8_t hdr[] = { FWP_SOF, FWP_CMD_DATA, 0x01, 0x10 }; /* LEN = 4097 */

    fwp_parser_init(&p);
    CHECK(feed_all(&p, hdr, sizeof(hdr)) == FWP_PARSE_LEN_ERROR);
    CHECK(feed_all(&p, REF_PING, sizeof(REF_PING)) == FWP_PARSE_FRAME);
}

int main(void)
{
    test_crc_reference_values();
    test_crc32_is_chainable();
    test_encode_matches_reference();
    test_encode_rejects_bad_sizes();
    test_parse_roundtrip();
    test_parse_skips_noise_before_frame();
    test_parse_detects_corruption();
    test_parse_rejects_oversized_length();

    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
