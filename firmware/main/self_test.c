/**
 * @file self_test.c
 * @brief Checks run once after an update, before the image is confirmed.
 *
 * Rule: a new firmware is only marked valid if it can still receive the next
 * update. Add product-specific checks here (sensor answers, Wi-Fi connects...).
 */
#include "self_test.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "uart_updater.h"

#define MIN_FREE_HEAP_BYTES (20 * 1024)
#define UPDATER_WAIT_MS     1000

static const char *TAG = "self_test";

static bool check_heap(void)
{
    const uint32_t free_heap = esp_get_free_heap_size();
    const bool     ok        = free_heap >= MIN_FREE_HEAP_BYTES;
    ESP_LOGI(TAG, "[%s] free heap: %lu bytes", ok ? "PASS" : "FAIL", (unsigned long)free_heap);
    return ok;
}

static bool check_updater(void)
{
    for (int waited = 0; !uart_updater_is_running() && waited < UPDATER_WAIT_MS; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const bool ok = uart_updater_is_running();
    ESP_LOGI(TAG, "[%s] UART updater running", ok ? "PASS" : "FAIL");
    return ok;
}

bool self_test_run(void)
{
    bool ok = true;

    ok &= check_heap();
    ok &= check_updater();

#if CONFIG_DEMO_SIMULATE_BROKEN_FIRMWARE
    ESP_LOGE(TAG, "[FAIL] simulated failure (CONFIG_DEMO_SIMULATE_BROKEN_FIRMWARE)");
    ok = false;
#endif

    return ok;
}
