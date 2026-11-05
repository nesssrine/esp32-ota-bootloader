/**
 * @file uart_updater.h
 * @brief Receives a new application image over UART and writes it to the
 *        inactive OTA partition (A/B update).
 */
#pragma once

#include <stdbool.h>

/** Configure the UART and start the updater task. */
void uart_updater_start(void);

/** True once the updater task is up and listening. */
bool uart_updater_is_running(void);
