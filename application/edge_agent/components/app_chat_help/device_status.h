/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <time.h>

#include "esp_err.h"

typedef struct {
    struct tm local_time;
    char date[11];
    char ip[16];
    int cpu_percent;
    size_t internal_total;
    size_t internal_free;
    size_t psram_total;
    size_t psram_free;
} app_chat_help_status_t;

esp_err_t app_chat_help_status_init(void);
void app_chat_help_get_status(app_chat_help_status_t *out);
