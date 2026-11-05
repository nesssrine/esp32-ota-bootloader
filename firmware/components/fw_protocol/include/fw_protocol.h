/**
 * @file fw_protocol.h
 * @brief Framed binary protocol used to transfer a firmware image over UART.
 *
 * Frame layout (all multi-byte fields are little-endian):
 *
 *   +------+-----+---------+-------------------+---------+
 *   | SOF  | CMD | LEN     | PAYLOAD (LEN B)   | CRC16   |
 *   | 0xA5 | 1 B | 2 B     | 0..1024 B         | 2 B     |
 *   +------+-----+---------+-------------------+---------+
 *
 * CRC16 is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) computed over
 * CMD + LEN + PAYLOAD. The whole image is additionally protected by a
 * zlib-compatible CRC32 announced in the START command.
 *
 * This module is pure C99 with no ESP-IDF dependency, so it can be
 * unit-tested on a host PC (see tests/host).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FWP_SOF             0xA5u
#define FWP_RSP_FLAG        0x80u   /**< Responses echo the command with this bit set */
#define FWP_RSP_NACK        0xFFu   /**< Response to a frame that could not be parsed */
#define FWP_MAX_PAYLOAD     1024u
#define FWP_DATA_HDR_SIZE   4u      /**< DATA payload starts with a u32 offset */
#define FWP_MAX_DATA        (FWP_MAX_PAYLOAD - FWP_DATA_HDR_SIZE)
#define FWP_OVERHEAD        6u      /**< SOF + CMD + LEN(2) + CRC(2) */
#define FWP_MAX_FRAME       (FWP_MAX_PAYLOAD + FWP_OVERHEAD)

/** Commands sent by the host. */
typedef enum {
    FWP_CMD_PING   = 0x01, /**< Liveness check, empty payload */
    FWP_CMD_START  = 0x02, /**< u32 image_size, u32 image_crc32 */
    FWP_CMD_DATA   = 0x03, /**< u32 offset, then up to FWP_MAX_DATA bytes */
    FWP_CMD_END    = 0x04, /**< Verify image and select it for next boot */
    FWP_CMD_REBOOT = 0x05, /**< Restart the device */
    FWP_CMD_INFO   = 0x06, /**< Returns running partition / version as text */
} fwp_cmd_t;

/** First payload byte of every response. */
typedef enum {
    FWP_OK              = 0x00,
    FWP_ERR_CRC         = 0x01, /**< Frame CRC16 or image CRC32 mismatch */
    FWP_ERR_STATE       = 0x02, /**< Command not valid in current state */
    FWP_ERR_LEN         = 0x03, /**< Bad payload length or image size */
    FWP_ERR_OFFSET      = 0x04, /**< DATA offset is not the expected one */
    FWP_ERR_FLASH       = 0x05, /**< Flash erase/write failed */
    FWP_ERR_IMAGE       = 0x06, /**< Not a valid ESP32 application image */
    FWP_ERR_UNKNOWN_CMD = 0x07,
    FWP_ERR_REJECTED    = 0x08, /**< Image refused (e.g. version that was rolled back) */
} fwp_status_t;

typedef struct {
    uint8_t  cmd;
    uint16_t len;
    uint8_t  payload[FWP_MAX_PAYLOAD];
} fwp_frame_t;

typedef enum {
    FWP_PARSE_INCOMPLETE = 0, /**< Need more bytes */
    FWP_PARSE_FRAME,          /**< A valid frame is available in parser->frame */
    FWP_PARSE_CRC_ERROR,      /**< Frame received but CRC16 mismatch */
    FWP_PARSE_LEN_ERROR,      /**< LEN field larger than FWP_MAX_PAYLOAD */
} fwp_parse_result_t;

typedef struct {
    int         state;
    uint16_t    index;
    uint16_t    crc_rx;
    fwp_frame_t frame;
} fwp_parser_t;

/** Reset the parser so it waits for a new SOF byte. */
void fwp_parser_init(fwp_parser_t *parser);

/**
 * Feed one received byte to the parser (byte-oriented state machine).
 * Bytes before a SOF are ignored, so the parser resynchronises on its own
 * after noise or log output on the same line.
 */
fwp_parse_result_t fwp_parser_feed(fwp_parser_t *parser, uint8_t byte);

/**
 * Encode a frame into @p out.
 * @return number of bytes written, or 0 if @p len or @p out_size is invalid.
 */
size_t fwp_encode(uint8_t cmd, const uint8_t *payload, uint16_t len,
                  uint8_t *out, size_t out_size);

/** CRC-16/CCITT-FALSE, chainable: start with crc = 0xFFFF. */
uint16_t fwp_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

/** CRC-32 (zlib/IEEE 802.3), chainable: start with crc = 0. */
uint32_t fwp_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

static inline uint32_t fwp_get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void fwp_put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#ifdef __cplusplus
}
#endif
