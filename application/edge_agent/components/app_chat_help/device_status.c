/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "device_status.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#define CPU_SAMPLE_US 1000000

static portMUX_TYPE s_cpu_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_cpu_percent = -1;

static void sample_cpu(void *arg)
{
    (void)arg;
    static int64_t previous_us;
    static configRUN_TIME_COUNTER_TYPE previous_idle[configNUMBER_OF_CORES];
    int64_t now = esp_timer_get_time();
    uint64_t idle = 0;

    for (BaseType_t core = 0; core < configNUMBER_OF_CORES; ++core) {
#if CONFIG_FREERTOS_SMP
        configRUN_TIME_COUNTER_TYPE current = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandleForCore(core));
#else
        configRUN_TIME_COUNTER_TYPE current = ulTaskGetIdleRunTimeCounterForCore(core);
#endif
        idle += (configRUN_TIME_COUNTER_TYPE)(current - previous_idle[core]);
        previous_idle[core] = current;
    }

    if (previous_us > 0 && now > previous_us) {
        uint64_t capacity = (uint64_t)(now - previous_us) * configNUMBER_OF_CORES;
        /* Idle time is credited at a context switch and can straddle samples. */
        int busy = idle >= capacity ? 0 : (int)(100 - (idle * 100 + capacity / 2) / capacity);
        portENTER_CRITICAL(&s_cpu_lock);
        s_cpu_percent = busy;
        portEXIT_CRITICAL(&s_cpu_lock);
    }
    previous_us = now;
}
#endif

esp_err_t app_chat_help_status_init(void)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    esp_timer_handle_t timer = NULL;
    esp_err_t err = esp_timer_create(&(esp_timer_create_args_t) {
        .callback = sample_cpu,
        .name = "chat_help_cpu",
        .skip_unhandled_events = true,
    }, &timer);
    if (err == ESP_OK) {
        sample_cpu(NULL);
        err = esp_timer_start_periodic(timer, CPU_SAMPLE_US);
        if (err != ESP_OK) {
            esp_timer_delete(timer);
        }
    }
    return err;
#else
    return ESP_OK;
#endif
}

void app_chat_help_get_status(app_chat_help_status_t *out)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    time_t now = time(NULL);
    wifi_manager_status_t wifi = {0};

    memset(out, 0, sizeof(*out));
    if (localtime_r(&now, &out->local_time) && out->local_time.tm_year >= 120) {
        strftime(out->date, sizeof(out->date), "%Y-%m-%d", &out->local_time);
    }
    wifi_manager_get_status(&wifi);
    const char *ip = wifi.sta_connected ? wifi.sta_ip : (wifi.ap_active ? wifi.ap_ip : NULL);
    if (ip && strcmp(ip, "0.0.0.0") != 0) {
        strlcpy(out->ip, ip, sizeof(out->ip));
    }
    out->cpu_percent = -1;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    portENTER_CRITICAL(&s_cpu_lock);
    out->cpu_percent = s_cpu_percent;
    portEXIT_CRITICAL(&s_cpu_lock);
#endif
    out->internal_total = heap_caps_get_total_size(internal_caps);
    out->internal_free = heap_caps_get_free_size(internal_caps);
    out->psram_total = heap_caps_get_total_size(psram_caps);
    out->psram_free = heap_caps_get_free_size(psram_caps);
}
