/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "token_usage.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "llm/claw_llm_http_transport.h"
#include "nvs.h"

#define TOKEN_USAGE_NAMESPACE  "claw_usage"
#define TOKEN_USAGE_KEY        "totals_v1"
#define TOKEN_FLUSH_INTERVAL   (60 * 1000000LL)
#define TOKEN_MAX_JSON_INTEGER 9007199254740991.0

typedef struct {
    uint32_t date;
    uint32_t reserved;
    app_token_period_t today;
    app_token_period_t month;
} token_usage_store_t;

static const char *TAG = "app_token_usage";
static SemaphoreHandle_t s_lock;
static token_usage_store_t s_totals;
static nvs_handle_t s_nvs;
static esp_timer_handle_t s_flush_timer;
static int64_t s_last_write;
static bool s_dirty;
static bool s_durable;

static uint32_t date_key(const struct tm *date)
{
    return (date->tm_year + 1900) * 10000 + (date->tm_mon + 1) * 100 + date->tm_mday;
}

static esp_err_t flush_locked(void)
{
    if (!s_nvs || !s_dirty) {
        return ESP_OK;
    }
    esp_err_t err = nvs_set_blob(s_nvs, TOKEN_USAGE_KEY, &s_totals, sizeof(s_totals));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    s_last_write = esp_timer_get_time();
    s_durable = (err == ESP_OK);
    if (err == ESP_OK) {
        s_dirty = false;
    } else {
        ESP_LOGW(TAG, "Could not save token usage: %s", esp_err_to_name(err));
    }
    return err;
}

static void flush_tokens(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (flush_locked() != ESP_OK) {
        esp_timer_start_once(s_flush_timer, TOKEN_FLUSH_INTERVAL);
    }
    xSemaphoreGive(s_lock);
}

static void flush_before_restart(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    flush_locked();
    xSemaphoreGive(s_lock);
}

static bool usage_value(const cJSON *usage, const char *name, uint64_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(usage, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    double value = item->valuedouble;
    if (!isfinite(value) || value < 0 || value > TOKEN_MAX_JSON_INTEGER) {
        return false;
    }
    uint64_t integer = (uint64_t)value;
    if ((double)integer != value) {
        return false;
    }
    *out = integer;
    return true;
}

static bool response_token_count(const char *body, uint64_t *out)
{
    cJSON *root = cJSON_Parse(body);
    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    uint64_t input = 0;
    uint64_t output = 0;
    bool reported = usage_value(usage, "total_tokens", out);

    if (!reported && usage_value(usage, "prompt_tokens", &input) &&
            usage_value(usage, "completion_tokens", &output)) {
        *out = input + output;
        reported = true;
    } else if (!reported && usage_value(usage, "input_tokens", &input) &&
               usage_value(usage, "output_tokens", &output)) {
        /* Anthropic reports the cache portions separately. OpenAI Responses
         * includes them in input_tokens and normally supplies total_tokens. */
        uint64_t cache_read = 0;
        uint64_t cache_created = 0;
        usage_value(usage, "cache_read_input_tokens", &cache_read);
        usage_value(usage, "cache_creation_input_tokens", &cache_created);
        *out = input + output + cache_read + cache_created;
        reported = true;
    }
    cJSON_Delete(root);
    return reported;
}

static void record_response(const char *body)
{
    struct tm date;
    uint64_t tokens = 0;
    bool reported = response_token_count(body, &tokens);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    time_t now = time(NULL);
    if (!localtime_r(&now, &date) || date.tm_year < 120) {
        xSemaphoreGive(s_lock);
        return;
    }
    uint32_t key = date_key(&date);

    bool new_day = key != s_totals.date;
    if (key / 100 != s_totals.date / 100) {
        memset(&s_totals.month, 0, sizeof(s_totals.month));
    }
    if (new_day) {
        memset(&s_totals.today, 0, sizeof(s_totals.today));
        s_totals.date = key;
    }
    if (reported) {
        s_totals.today.tokens += tokens;
        s_totals.month.tokens += tokens;
        s_totals.today.reported++;
        s_totals.month.reported++;
    } else {
        s_totals.today.unreported++;
        s_totals.month.unreported++;
    }
    s_dirty = true;
    /* Model tasks can have PSRAM stacks. Write Flash from the timer task. */
    if (s_flush_timer && !esp_timer_is_active(s_flush_timer)) {
        int64_t remaining = TOKEN_FLUSH_INTERVAL - (esp_timer_get_time() - s_last_write);
        if (new_day || !s_last_write || remaining <= 0) {
            remaining = 1;
        }
        esp_err_t err = esp_timer_start_once(s_flush_timer, remaining);
        if (err != ESP_OK) {
            s_durable = false;
            ESP_LOGW(TAG, "Could not schedule token save: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t app_token_usage_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nvs_open(TOKEN_USAGE_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err == ESP_OK) {
        size_t size = sizeof(s_totals);
        err = nvs_get_blob(s_nvs, TOKEN_USAGE_KEY, &s_totals, &size);
        if (err != ESP_OK || size != sizeof(s_totals)) {
            memset(&s_totals, 0, sizeof(s_totals));
        }
        s_durable = err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
        err = esp_timer_create(&(esp_timer_create_args_t) {
            .callback = flush_tokens,
            .name = "claw_token_save",
        }, &s_flush_timer);
        if (err != ESP_OK) {
            s_durable = false;
            ESP_LOGW(TAG, "Could not create token save timer: %s", esp_err_to_name(err));
        }
        err = esp_register_shutdown_handler(flush_before_restart);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not register token save on restart: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Token usage is volatile: %s", esp_err_to_name(err));
    }

    claw_llm_http_set_response_observer(record_response);
    return ESP_OK;
}

void app_token_usage_snapshot(const struct tm *date, app_token_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!date || !s_lock) {
        return;
    }
    uint32_t key = date_key(date);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->available = true;
    out->durable = s_durable;
    if (key == s_totals.date) {
        out->today = s_totals.today;
    }
    if (key / 100 == s_totals.date / 100) {
        out->month = s_totals.month;
    }
    xSemaphoreGive(s_lock);
}
