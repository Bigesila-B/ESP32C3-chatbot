/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_web_search.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_crt_bundle.h"
#include "claw_http_transfer.h"
#include "esp_log.h"

static const char *TAG = "cap_web_search";

/* Maximum wire response including the terminator. */
#define CAP_WEB_SEARCH_BUF_MAX      (12 * 1024)
#define CAP_WEB_SEARCH_RESULT_COUNT 3

typedef enum {
    CAP_WEB_SEARCH_PROVIDER_NONE = 0,
    CAP_WEB_SEARCH_PROVIDER_BRAVE,
    CAP_WEB_SEARCH_PROVIDER_TAVILY,
} cap_web_search_provider_t;

typedef struct {
    char *prefix;       /* Borrowed caller output until the JSON has been parsed. */
    size_t prefix_limit; /* Payload bytes, excluding its terminator. */
    char *tail;
    size_t len;
    esp_err_t error;
} cap_web_search_buf_t;

typedef struct {
    char brave_key[128];
    char tavily_key[128];
    cap_web_search_provider_t provider;
} cap_web_search_state_t;

static EXT_RAM_BSS_ATTR cap_web_search_state_t s_search = {0};

static void cap_web_search_refresh_provider(void)
{
    if (s_search.tavily_key[0]) {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_TAVILY;
    } else if (s_search.brave_key[0]) {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_BRAVE;
    } else {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_NONE;
    }
}

static esp_err_t cap_web_search_http_event_handler(esp_http_client_event_t *event)
{
    if (!event || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    cap_web_search_buf_t *buf = event->user_data;
    const char *data = event->data;
    size_t count = (size_t)event->data_len;
    if (buf->error != ESP_OK) {
        return buf->error;
    }
    if (count > CAP_WEB_SEARCH_BUF_MAX - 1 - buf->len) {
        buf->error = ESP_ERR_INVALID_SIZE;
        return buf->error;
    }
    if (buf->len < buf->prefix_limit) {
        size_t available = buf->prefix_limit - buf->len;
        size_t n = count < available ? count : available;
        memcpy(buf->prefix + buf->len, data, n);
        buf->len += n;
        buf->prefix[buf->len] = '\0';
        data += n;
        count -= n;
    }
    if (count) {
        if (!buf->tail) {
            buf->tail = malloc(CAP_WEB_SEARCH_BUF_MAX - 1 - buf->prefix_limit);
            if (!buf->tail) {
                buf->error = ESP_ERR_NO_MEM;
                return buf->error;
            }
        }
        memcpy(buf->tail + (buf->len - buf->prefix_limit), data, count);
        buf->len += count;
    }
    return ESP_OK;
}

/* Both providers have closed their TLS client by the time this is called. */
static esp_err_t cap_web_search_parse_body(cap_web_search_buf_t *buf, cJSON **root)
{
    if (!buf->tail) {
        *root = cJSON_Parse(buf->prefix);
    } else {
        char *joined = malloc(buf->len + 1);
        if (!joined) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(joined, buf->prefix, buf->prefix_limit);
        memcpy(joined + buf->prefix_limit, buf->tail, buf->len - buf->prefix_limit);
        joined[buf->len] = '\0';
        free(buf->tail);
        buf->tail = NULL;
        *root = cJSON_Parse(joined);
        free(joined);
    }
    return *root ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static size_t cap_web_search_url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    if (!src || !dst || dst_size == 0) {
        return 0;
    }

    while (*src && pos < dst_size - 1) {
        unsigned char c = (unsigned char) * src;

        if ((c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = (char)c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            if (pos + 3 >= dst_size) {
                break;
            }
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
        src++;
    }

    dst[pos] = '\0';
    return pos;
}

/* 按 UTF-8 码点算一个字符的长度；非法首字节或截断序列都按单字节跳过，
 * 避免 memcpy 越过字符串结尾，也避免把半个字符塞进 JSON 请求体。 */
static size_t cap_web_search_utf8_char_len(const unsigned char *p)
{
    unsigned char c = p[0];
    size_t step;

    if (c < 0x80) {
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        step = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        step = 3;
    } else if (c >= 0xF0 && c <= 0xF4) {
        step = 4;
    } else {
        return 1;
    }
    for (size_t k = 1; k < step; k++) {
        if (p[k] == '\0' || (p[k] & 0xC0) != 0x80) {
            return 1;
        }
    }
    return step;
}

/* 搜索结果正文按 UTF-8 字符边界截成短摘要。
 * 完整正文会在第二轮 LLM 请求里再被复制一遍，C3 上很容易把堆撑满；
 * 200 字节（约 60 个汉字）足够模型判断这条结果是否相关。 */
static void cap_web_search_excerpt(const char *src, char *dst, size_t dst_size)
{
    /* 摘要只用来让模型判断相关性，140 字节（约 45 个汉字）足够；
     * 结果会再进一次模型请求，这里省下的每个字节都要乘以二。 */
    const size_t limit = 140;
    size_t off = 0;

    dst[0] = '\0';
    if (!src || dst_size < 8) {
        return;
    }
    while (*src) {
        size_t step = cap_web_search_utf8_char_len((const unsigned char *)src);

        if (off + step > limit || off + step + 4 >= dst_size) {
            break;
        }
        memcpy(dst + off, src, step);
        off += step;
        src += step;
    }
    dst[off] = '\0';
    if (*src) {
        memcpy(dst + off, "...", 3);
        dst[off + 3] = '\0';
    }
}

static void cap_web_search_format_brave_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *web = NULL;
    cJSON *results = NULL;
    cJSON *item = NULL;
    size_t offset = 0;
    int index = 0;

    web = cJSON_GetObjectItem(root, "web");
    results = web ? cJSON_GetObjectItem(web, "results") : NULL;
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON_ArrayForEach(item, results) {
        cJSON *title = NULL;
        cJSON *url = NULL;
        cJSON *description = NULL;
        char excerpt[256];
        int written;

        if (index >= CAP_WEB_SEARCH_RESULT_COUNT || offset >= output_size - 1) {
            break;
        }

        title = cJSON_GetObjectItem(item, "title");
        url = cJSON_GetObjectItem(item, "url");
        description = cJSON_GetObjectItem(item, "description");
        cap_web_search_excerpt(cJSON_IsString(description) ? description->valuestring : "",
                               excerpt, sizeof(excerpt));
        written = snprintf(output + offset,
                           output_size - offset,
                           "%d. %s\n   %s\n   %s\n\n",
                           index + 1,
                           cJSON_IsString(title) ? title->valuestring : "(no title)",
                           cJSON_IsString(url) ? url->valuestring : "",
                           excerpt);
        if (written < 0 || (size_t)written >= output_size - offset) {
            output[output_size - 1] = '\0';
            return;
        }

        offset += (size_t)written;
        index++;
    }
}

static void cap_web_search_format_tavily_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *results = NULL;
    cJSON *item = NULL;
    size_t offset = 0;
    int index = 0;

    results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON_ArrayForEach(item, results) {
        cJSON *title = NULL;
        cJSON *url = NULL;
        cJSON *content = NULL;
        char excerpt[256];
        int written;

        if (index >= CAP_WEB_SEARCH_RESULT_COUNT || offset >= output_size - 1) {
            break;
        }

        title = cJSON_GetObjectItem(item, "title");
        url = cJSON_GetObjectItem(item, "url");
        content = cJSON_GetObjectItem(item, "content");
        cap_web_search_excerpt(cJSON_IsString(content) ? content->valuestring : "",
                               excerpt, sizeof(excerpt));
        written = snprintf(output + offset,
                           output_size - offset,
                           "%d. %s\n   %s\n   %s\n\n",
                           index + 1,
                           cJSON_IsString(title) ? title->valuestring : "(no title)",
                           cJSON_IsString(url) ? url->valuestring : "",
                           excerpt);
        if (written < 0 || (size_t)written >= output_size - offset) {
            output[output_size - 1] = '\0';
            return;
        }

        offset += (size_t)written;
        index++;
    }
}

static char *cap_web_search_build_tavily_payload(const char *query)
{
    cJSON *root = NULL;
    char *payload = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "query", query);
    cJSON_AddNumberToObject(root, "max_results", CAP_WEB_SEARCH_RESULT_COUNT);
    cJSON_AddBoolToObject(root, "include_answer", false);
    cJSON_AddStringToObject(root, "search_depth", "basic");
    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static esp_err_t cap_web_search_brave_direct(const char *url, cap_web_search_buf_t *buf)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = cap_web_search_http_event_handler,
        .user_data = buf,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
#ifdef CONFIG_HTTP_REUSE_ENABLE
        .keep_alive_enable = true,
#endif
    };
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status;

    client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Subscription-Token", s_search.brave_key);
    err = claw_http_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Brave search returned %d", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_web_search_tavily_direct(const char *query, cap_web_search_buf_t *buf)
{
    esp_http_client_config_t config = {
        .url = "https://api.tavily.com/search",
        .event_handler = cap_web_search_http_event_handler,
        .user_data = buf,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
#ifdef CONFIG_HTTP_REUSE_ENABLE
        .keep_alive_enable = true,
#endif
    };
    esp_http_client_handle_t client = NULL;
    char auth[192];
    char *payload = NULL;
    esp_err_t err;
    int status;

    payload = cap_web_search_build_tavily_payload(query);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    client = esp_http_client_init(&config);
    if (!client) {
        free(payload);
        return ESP_FAIL;
    }

    snprintf(auth, sizeof(auth), "Bearer %s", s_search.tavily_key);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, payload, strlen(payload));
    err = claw_http_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(payload);
    if (err != ESP_OK) {
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Tavily search returned %d", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_web_search_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    cJSON *input = NULL;
    cJSON *query = NULL;
    cap_web_search_buf_t buf = {0};
    cJSON *root = NULL;
    esp_err_t err = ESP_OK;

    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_web_search_refresh_provider();
    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_NONE) {
        snprintf(output, output_size, "Error: no search provider credentials configured");
        return ESP_ERR_INVALID_STATE;
    }

    input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    query = cJSON_GetObjectItem(input, "query");
    if (!cJSON_IsString(query) || !query->valuestring || !query->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: missing query");
        return ESP_ERR_INVALID_ARG;
    }

    output[0] = '\0';
    buf.prefix = output;
    buf.prefix_limit = (output_size < CAP_WEB_SEARCH_BUF_MAX ? output_size : CAP_WEB_SEARCH_BUF_MAX) - 1;

    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_TAVILY) {
        err = cap_web_search_tavily_direct(query->valuestring, &buf);
    } else {
        char encoded_query[256];
        char url[512];

        cap_web_search_url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
        snprintf(url,
                 sizeof(url),
                 "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d",
                 encoded_query,
                 CAP_WEB_SEARCH_RESULT_COUNT);
        err = cap_web_search_brave_direct(url, &buf);
    }

    cJSON_Delete(input);
    if (buf.error != ESP_OK) {
        err = buf.error;
    }
    if (err == ESP_OK) {
        err = cap_web_search_parse_body(&buf, &root);
    }
    free(buf.tail);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_SIZE) {
            snprintf(output, output_size, "Error: search response exceeds %d bytes", CAP_WEB_SEARCH_BUF_MAX);
        } else {
            snprintf(output, output_size, "Error: search request failed (%s)", esp_err_to_name(err));
        }
        return err;
    }

    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_TAVILY) {
        cap_web_search_format_tavily_results(root, output, output_size);
    } else {
        cap_web_search_format_brave_results(root, output, output_size);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_web_search_descriptors[] = {
    {
        .id = "web_search",
        .name = "web_search",
        .family = "system",
        .description = "Search the web with the configured provider and return concise formatted results.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}",
        .execute = cap_web_search_execute,
    },
};

static const claw_cap_group_t s_web_search_group = {
    .group_id = "cap_web_search",
    .descriptors = s_web_search_descriptors,
    .descriptor_count = sizeof(s_web_search_descriptors) / sizeof(s_web_search_descriptors[0]),
};

esp_err_t cap_web_search_register_group(void)
{
    if (claw_cap_group_exists(s_web_search_group.group_id)) {
        return ESP_OK;
    }

    cap_web_search_refresh_provider();
    return claw_cap_register_group(&s_web_search_group);
}

esp_err_t cap_web_search_set_brave_key(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_search.brave_key, api_key, sizeof(s_search.brave_key));
    cap_web_search_refresh_provider();
    return ESP_OK;
}

esp_err_t cap_web_search_set_tavily_key(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_search.tavily_key, api_key, sizeof(s_search.tavily_key));
    cap_web_search_refresh_provider();
    return ESP_OK;
}
