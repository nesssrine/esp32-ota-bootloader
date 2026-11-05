/**
 * @file self_test.h
 * @brief Post-update health check deciding between "keep" and "roll back".
 */
#pragma once

#include <stdbool.h>

/** Run all checks. Returns true if the new firmware can be marked valid. */
bool self_test_run(void);
