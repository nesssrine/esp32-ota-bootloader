/**
 * @file uart_updater.c
 * @brief UART firmware update session (stop-and-wait, one ACK per frame).
 *
 *   IDLE --START--> RECEIVING --DATA...--> RECEIVING --END--> IDLE
 *                       |                                       (image selected
 *                       +--timeout / error--> abort --> IDLE     for next boot)
 *
 * The image is always written to the *inactive* OTA slot, so an interrupted
 * transfer (power loss, cable removed) never damages the running firmware.
 */
#include "uart_updater.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_app_desc.h"   /* esp_app_desc_t, esp_app_get_description() */
#include "esp_app_format.h" /* esp_image_header_t (bootloader_support)  */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "fw_protocol.h"

#define UPD_UART             CONFIG_UPDATER_UART_PORT_NUM
#define UART_RX_BUF_SIZE     4096
#define SESSION_TIMEOUT_MS   10000
#define UPDATER_TASK_STACK   4096
#define UPDATER_TASK_PRIO    5

/* Bytes needed at the start of an image to reach the app description. */
#define IMAGE_DESC_END (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + \
                        sizeof(esp_app_desc_t))

static const char *TAG = "updater";

typedef struct {
    bool                   active;
    esp_ota_handle_t       handle;
    const esp_partition_t *target;
    uint32_t               image_size;
    uint32_t               image_crc;
    uint32_t               received;
    uint32_t               crc;
    TickType_t             last_activity;
} session_t;

static session_t     s_session;
static fwp_parser_t  s_parser;
static uint8_t       s_tx[FWP_MAX_FRAME];
static volatile bool s_running;

/* When the updater shares the console UART, log text would be mixed with
 * protocol frames. The host parser resynchronises on SOF + CRC anyway, but
 * muting logs during a transfer keeps the link clean and fast. */
static void set_quiet(bool quiet)
{
#if defined(CONFIG_ESP_CONSOLE_UART_NUM) && (CONFIG_UPDATER_UART_PORT_NUM == CONFIG_ESP_CONSOLE_UART_NUM)
    esp_log_level_set("*", quiet ? ESP_LOG_NONE : (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);
#else
    (void)quiet;
#endif
}

static void send_response(uint8_t cmd, uint8_t status, const char *text)
{
    uint8_t payload[1 + 96];
    size_t  text_len = text ? strnlen(text, sizeof(payload) - 1) : 0;

    payload[0] = status;
    if (text_len > 0) {
        memcpy(&payload[1], text, text_len);
    }

    size_t n = fwp_encode(cmd == FWP_RSP_NACK ? FWP_RSP_NACK : (uint8_t)(cmd | FWP_RSP_FLAG),
                          payload, (uint16_t)(1 + text_len), s_tx, sizeof(s_tx));
    uart_write_bytes(UPD_UART, s_tx, n);
}

static void session_abort(const char *reason)
{
    if (!s_session.active) {
        return;
    }
    esp_ota_abort(s_session.handle);
    s_session.active = false;
    set_quiet(false);
    ESP_LOGW(TAG, "Update aborted: %s", reason);
}

/* Check the first chunk really is an ESP32 application before writing it. */
static fwp_status_t validate_image_start(const uint8_t *data, size_t len)
{
    if (len < IMAGE_DESC_END) {
        return FWP_ERR_IMAGE;
    }

    esp_image_header_t hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC || hdr.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return FWP_ERR_IMAGE;
    }

    esp_app_desc_t desc;
    memcpy(&desc, data + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return FWP_ERR_IMAGE;
    }

    /* Refuse the exact version that was just rolled back: it is known bad. */
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t         invalid_desc;
    if (invalid != NULL &&
        esp_ota_get_partition_description(invalid, &invalid_desc) == ESP_OK &&
        strncmp(invalid_desc.version, desc.version, sizeof(desc.version)) == 0) {
        return FWP_ERR_REJECTED;
    }
    return FWP_OK;
}

static fwp_status_t handle_start(const uint8_t *payload, uint16_t len)
{
    if (len != 8) {
        return FWP_ERR_LEN;
    }
    session_abort("restarted by host");

    const uint32_t         size   = fwp_get_u32_le(&payload[0]);
    const uint32_t         crc    = fwp_get_u32_le(&payload[4]);
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);

    if (target == NULL) {
        return FWP_ERR_FLASH;
    }
    if (size == 0 || size > target->size) {
        return FWP_ERR_LEN;
    }

    /* Sequential mode erases flash sector by sector while writing, so no
     * single command blocks long enough to trigger a host timeout. */
    if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &s_session.handle) != ESP_OK) {
        return FWP_ERR_FLASH;
    }

    ESP_LOGI(TAG, "Receiving %" PRIu32 " bytes into partition '%s'", size, target->label);
    s_session.active     = true;
    s_session.target     = target;
    s_session.image_size = size;
    s_session.image_crc  = crc;
    s_session.received   = 0;
    s_session.crc        = 0;
    set_quiet(true);
    return FWP_OK;
}

static fwp_status_t handle_data(const uint8_t *payload, uint16_t len)
{
    if (!s_session.active) {
        return FWP_ERR_STATE;
    }
    if (len <= FWP_DATA_HDR_SIZE) {
        return FWP_ERR_LEN;
    }

    const uint32_t offset = fwp_get_u32_le(payload);
    const uint8_t *data   = payload + FWP_DATA_HDR_SIZE;
    const size_t   n      = len - FWP_DATA_HDR_SIZE;

    /* Host re-sent the last chunk because our ACK was lost: already written. */
    if (offset + n == s_session.received) {
        return FWP_OK;
    }
    if (offset != s_session.received) {
        return FWP_ERR_OFFSET;
    }
    if (s_session.received + n > s_session.image_size) {
        return FWP_ERR_LEN;
    }

    if (offset == 0) {
        fwp_status_t st = validate_image_start(data, n);
        if (st != FWP_OK) {
            session_abort(st == FWP_ERR_REJECTED ? "version was rolled back before"
                                                 : "not a valid ESP32 image");
            return st;
        }
    }

    if (esp_ota_write(s_session.handle, data, n) != ESP_OK) {
        session_abort("flash write failed");
        return FWP_ERR_FLASH;
    }
    s_session.crc = fwp_crc32_update(s_session.crc, data, n);
    s_session.received += n;
    return FWP_OK;
}

static fwp_status_t handle_end(void)
{
    if (!s_session.active) {
        return FWP_ERR_STATE;
    }
    if (s_session.received != s_session.image_size) {
        session_abort("image incomplete");
        return FWP_ERR_LEN;
    }
    if (s_session.crc != s_session.image_crc) {
        session_abort("image CRC32 mismatch");
        return FWP_ERR_CRC;
    }

    s_session.active = false;
    set_quiet(false);

    /* esp_ota_end() also verifies the image structure and its SHA-256. */
    esp_err_t err = esp_ota_end(s_session.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image verification failed: %s", esp_err_to_name(err));
        return FWP_ERR_IMAGE;
    }
    err = esp_ota_set_boot_partition(s_session.target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot select boot partition: %s", esp_err_to_name(err));
        return FWP_ERR_FLASH;
    }
    ESP_LOGI(TAG, "Update OK, next boot from '%s'", s_session.target->label);
    return FWP_OK;
}

static void build_info(char *out, size_t size)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t  *app     = esp_app_get_description();
    esp_ota_img_states_t   state;
    const char            *state_str = "factory";

    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        state_str = (state == ESP_OTA_IMG_VALID)            ? "valid"
                    : (state == ESP_OTA_IMG_PENDING_VERIFY) ? "pending-verify"
                                                            : "other";
    }
    snprintf(out, size, "partition=%s version=%s state=%s", running->label, app->version,
             state_str);
}

static void handle_frame(const fwp_frame_t *f)
{
    char info[96];

    s_session.last_activity = xTaskGetTickCount();

    switch (f->cmd) {
    case FWP_CMD_PING:
        send_response(f->cmd, FWP_OK, NULL);
        break;
    case FWP_CMD_INFO:
        build_info(info, sizeof(info));
        send_response(f->cmd, FWP_OK, info);
        break;
    case FWP_CMD_START:
        send_response(f->cmd, handle_start(f->payload, f->len), NULL);
        break;
    case FWP_CMD_DATA:
        send_response(f->cmd, handle_data(f->payload, f->len), NULL);
        break;
    case FWP_CMD_END:
        send_response(f->cmd, handle_end(), NULL);
        break;
    case FWP_CMD_REBOOT:
        session_abort("reboot requested");
        send_response(f->cmd, FWP_OK, NULL);
        uart_wait_tx_done(UPD_UART, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        break;
    default:
        send_response(f->cmd, FWP_ERR_UNKNOWN_CMD, NULL);
        break;
    }
}

static void updater_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];

    fwp_parser_init(&s_parser);
    s_running = true;
    ESP_LOGI(TAG, "Listening on UART%d @ %d baud", UPD_UART, CONFIG_UPDATER_UART_BAUD_RATE);

    for (;;) {
        int n = uart_read_bytes(UPD_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));

        for (int i = 0; i < n; i++) {
            switch (fwp_parser_feed(&s_parser, buf[i])) {
            case FWP_PARSE_FRAME:
                handle_frame(&s_parser.frame);
                break;
            case FWP_PARSE_CRC_ERROR:
            case FWP_PARSE_LEN_ERROR:
                send_response(FWP_RSP_NACK, FWP_ERR_CRC, NULL); /* host will retransmit */
                break;
            default:
                break;
            }
        }

        if (s_session.active &&
            (xTaskGetTickCount() - s_session.last_activity) > pdMS_TO_TICKS(SESSION_TIMEOUT_MS)) {
            session_abort("timeout, host stopped sending");
        }
    }
}

void uart_updater_start(void)
{
    const uart_config_t cfg = {
        .baud_rate  = CONFIG_UPDATER_UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UPD_UART, UART_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UPD_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UPD_UART, CONFIG_UPDATER_UART_TX_PIN, CONFIG_UPDATER_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(updater_task, "uart_updater", UPDATER_TASK_STACK, NULL, UPDATER_TASK_PRIO, NULL);
}

bool uart_updater_is_running(void)
{
    return s_running;
}
