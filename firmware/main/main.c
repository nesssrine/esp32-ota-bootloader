/**
 * @file main.c
 * @brief Demo application: reports which partition/version is running,
 *        confirms (or rolls back) a fresh update, and blinks an LED at a
 *        per-build rate so the active firmware is visible at a glance.
 */
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "self_test.h"
#include "uart_updater.h"

static const char *TAG = "main";

static const char *ota_state_str(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

static void print_boot_info(void)
{
    const esp_app_desc_t  *app     = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot    = esp_ota_get_boot_partition();
    esp_ota_img_states_t   state;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " %s  v%s", app->project_name, app->version);
    ESP_LOGI(TAG, " built %s %s, ESP-IDF %s", app->date, app->time, app->idf_ver);
    ESP_LOGI(TAG, " running partition: %s @ 0x%06" PRIx32, running->label, running->address);
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        ESP_LOGI(TAG, " image state      : %s", ota_state_str(state));
    }
    if (boot != running) {
        ESP_LOGW(TAG, " boot partition is '%s' (a rollback happened)", boot ? boot->label : "?");
    }
    ESP_LOGI(TAG, "========================================");
}

/* A freshly updated image boots as PENDING_VERIFY. If it resets before being
 * confirmed (crash, watchdog...), the bootloader rolls back on its own. */
static void confirm_or_rollback(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t   state;

    if (esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return; /* factory image, or already confirmed */
    }

    ESP_LOGW(TAG, "New firmware is pending verification, running self-test...");
    if (self_test_run()) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Self-test passed: firmware confirmed");
    } else {
        ESP_LOGE(TAG, "Self-test failed: rolling back to previous firmware");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "Rollback impossible: no other valid firmware");
    }
}

static void blink_task(void *arg)
{
    (void)arg;
    const gpio_num_t led = (gpio_num_t)CONFIG_DEMO_LED_GPIO;
    bool             on  = false;

    gpio_reset_pin(led);
    gpio_set_direction(led, GPIO_MODE_OUTPUT);

    for (;;) {
        gpio_set_level(led, on);
        on = !on;
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DEMO_BLINK_PERIOD_MS / 2));
    }
}

void app_main(void)
{
    print_boot_info();
    uart_updater_start();
    confirm_or_rollback();
    xTaskCreate(blink_task, "blink", 2048, NULL, 3, NULL);
}
