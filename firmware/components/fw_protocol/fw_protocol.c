/**
 * @file fw_protocol.c
 * @brief Frame encoder / byte-oriented parser and CRC helpers.
 */
#include "fw_protocol.h"

#include <string.h>

enum {
    ST_WAIT_SOF = 0,
    ST_CMD,
    ST_LEN_L,
    ST_LEN_H,
    ST_PAYLOAD,
    ST_CRC_L,
    ST_CRC_H,
};

uint16_t fwp_crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint32_t fwp_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}

/* CRC16 over CMD + LEN(le) + PAYLOAD, shared by encoder and parser. */
static uint16_t frame_crc(uint8_t cmd, uint16_t len, const uint8_t *payload)
{
    const uint8_t hdr[3] = { cmd, (uint8_t)len, (uint8_t)(len >> 8) };
    uint16_t crc = fwp_crc16_update(0xFFFFu, hdr, sizeof(hdr));
    return fwp_crc16_update(crc, payload, len);
}

void fwp_parser_init(fwp_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
    parser->state = ST_WAIT_SOF;
}

fwp_parse_result_t fwp_parser_feed(fwp_parser_t *p, uint8_t byte)
{
    switch (p->state) {
    case ST_WAIT_SOF:
        if (byte == FWP_SOF) {
            p->state = ST_CMD;
        }
        break;

    case ST_CMD:
        p->frame.cmd = byte;
        p->state = ST_LEN_L;
        break;

    case ST_LEN_L:
        p->frame.len = byte;
        p->state = ST_LEN_H;
        break;

    case ST_LEN_H:
        p->frame.len |= (uint16_t)byte << 8;
        if (p->frame.len > FWP_MAX_PAYLOAD) {
            p->state = ST_WAIT_SOF;
            return FWP_PARSE_LEN_ERROR;
        }
        p->index = 0;
        p->state = (p->frame.len == 0) ? ST_CRC_L : ST_PAYLOAD;
        break;

    case ST_PAYLOAD:
        p->frame.payload[p->index++] = byte;
        if (p->index >= p->frame.len) {
            p->state = ST_CRC_L;
        }
        break;

    case ST_CRC_L:
        p->crc_rx = byte;
        p->state = ST_CRC_H;
        break;

    case ST_CRC_H:
        p->crc_rx |= (uint16_t)byte << 8;
        p->state = ST_WAIT_SOF;
        if (p->crc_rx != frame_crc(p->frame.cmd, p->frame.len, p->frame.payload)) {
            return FWP_PARSE_CRC_ERROR;
        }
        return FWP_PARSE_FRAME;

    default:
        p->state = ST_WAIT_SOF;
        break;
    }
    return FWP_PARSE_INCOMPLETE;
}

size_t fwp_encode(uint8_t cmd, const uint8_t *payload, uint16_t len,
                  uint8_t *out, size_t out_size)
{
    const size_t total = (size_t)len + FWP_OVERHEAD;
    if (len > FWP_MAX_PAYLOAD || out_size < total || (len > 0 && payload == NULL)) {
        return 0;
    }

    out[0] = FWP_SOF;
    out[1] = cmd;
    out[2] = (uint8_t)len;
    out[3] = (uint8_t)(len >> 8);
    if (len > 0) {
        memcpy(&out[4], payload, len);
    }
    const uint16_t crc = frame_crc(cmd, len, payload);
    out[4 + len] = (uint8_t)crc;
    out[5 + len] = (uint8_t)(crc >> 8);
    return total;
}
