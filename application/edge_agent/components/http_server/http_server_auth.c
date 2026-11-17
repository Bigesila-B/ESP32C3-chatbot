/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"

/*
 * 网页后台的最小认证：管理口令 + 内存态 Bearer token。
 *
 * 设计取舍（C3 上没有 TLS 服务端）：
 * - 口令只以加盐 SHA-256 存 NVS，绝不存明文，也不通过 /api/config 暴露。
 * - 不设出厂默认口令：首次打开后台强制由使用者自己设置，避免默认口令泄漏。
 * - token 只存在内存里，设备重启即失效；每次启动都要重新登录。
 * - 口令在局域网 HTTP 上是明文传输的（无 TLS），这是已知限制，写在 README 里。
 *
 * 例外（不需要 token）：GET /api/status、/api/auth、首页静态资源。
 * 首次设置时 /api/auth 同时接收 Wi-Fi 参数，否则新设备连上热点后没法配网。
 */
static const char *TAG = "http_auth";

#define AUTH_NVS_NAMESPACE   "espclaw_auth"
#define AUTH_NVS_KEY         "pwd_hash"
#define AUTH_PASSWORD_MIN    6
#define AUTH_PASSWORD_MAX    64
#define AUTH_TOKEN_BYTES     16
#define AUTH_TOKEN_TTL_US    (24LL * 60 * 60 * 1000 * 1000)

static char s_token[AUTH_TOKEN_BYTES * 2 + 1];
static int64_t s_token_expires_us;
static bool s_token_valid;

/* 等长字符串常量时间比较，避免按字节早退泄漏信息 */
static bool auth_streq_const(const char *a, const char *b)
{
    size_t len = strlen(a);
    unsigned diff = 0;

    /* 长度不同就直接拒绝，避免按 a 的长度去读更短的 b 造成越界 */
    if (len != strlen(b)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0;
}

static void auth_hex(const unsigned char *data, size_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* 固定盐 + 口令做 SHA-256；盐不保密，只是避免直接用通用彩虹表 */
static esp_err_t auth_password_hash(const char *password, char out[65])
{
    char salted[AUTH_PASSWORD_MAX + 32];
    unsigned char digest[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (!password || !info) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(salted, sizeof(salted), "espclaw-c3:%s", password);
    if (mbedtls_md(info, (const unsigned char *)salted, strlen(salted), digest) != 0) {
        return ESP_FAIL;
    }
    auth_hex(digest, sizeof(digest), out);
    return ESP_OK;
}

static esp_err_t auth_read_hash(char out[65])
{
    nvs_handle_t handle;
    size_t len = 65;
    esp_err_t err;

    err = nvs_open(AUTH_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, AUTH_NVS_KEY, out, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t auth_write_hash(const char *hash)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AUTH_NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, AUTH_NVS_KEY, hash);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool http_server_auth_is_configured(void)
{
    char hash[65];

    return auth_read_hash(hash) == ESP_OK && hash[0] != '\0';
}

/* 生成新 token；旧的立即失效 */
static const char *auth_issue_token(void)
{
    unsigned char raw[AUTH_TOKEN_BYTES];

    for (size_t i = 0; i < sizeof(raw); i++) {
        raw[i] = (unsigned char)(esp_random() & 0xFF);
    }
    auth_hex(raw, sizeof(raw), s_token);
    s_token_expires_us = esp_timer_get_time() + AUTH_TOKEN_TTL_US;
    s_token_valid = true;
    return s_token;
}

static bool auth_request_token_valid(httpd_req_t *req)
{
    char header[96];
    const char *candidate;

    if (!s_token_valid || esp_timer_get_time() >= s_token_expires_us) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return false;
    }
    if (strncmp(header, "Bearer ", 7) != 0) {
        return false;
    }
    candidate = header + 7;
    return auth_streq_const(s_token, candidate);
}

/*
 * 各敏感 handler 的第一行调用。失败时这里已经发出 401 响应，
 * 调用方直接 return ESP_OK，避免 httpd 再补发一次错误响应。
 */
bool http_server_auth_check(httpd_req_t *req)
{
    if (auth_request_token_valid(req)) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"error\":\"authentication required\"}");
    return false;
}

static esp_err_t auth_get_handler(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();

    if (!out) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "configured", http_server_auth_is_configured());
    cJSON_AddBoolToObject(out, "authenticated", auth_request_token_valid(req));
    return http_server_send_json_response(req, out);
}

static esp_err_t auth_bad_request(httpd_req_t *req, const char *message)
{
    cJSON *out = cJSON_CreateObject();

    if (!out) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", false);
    http_server_json_add_string(out, "error", message);
    httpd_resp_set_status(req, "400 Bad Request");
    return http_server_send_json_response(req, out);
}

static esp_err_t auth_post_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    const char *action = NULL;
    const char *password = NULL;
    char stored[65];
    char computed[65];
    esp_err_t err;

    err = http_server_parse_json_body(req, &root);
    if (err != ESP_OK || !root) {
        cJSON_Delete(root);
        return auth_bad_request(req, "Invalid JSON body");
    }
    action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "action"));
    password = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "password"));

    if (!action || !password) {
        cJSON_Delete(root);
        return auth_bad_request(req, "action and password are required");
    }
    size_t pass_len = strlen(password);
    if (pass_len < AUTH_PASSWORD_MIN || pass_len > AUTH_PASSWORD_MAX) {
        cJSON_Delete(root);
        return auth_bad_request(req, "password length must be 6..64");
    }

    /* 改密码必须先持有有效 token，避免任何人拿旧口令就能直接重置 */
    if (strcmp(action, "change") == 0 && !auth_request_token_valid(req)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "{\"error\":\"authentication required\"}");
    }

    bool configured = http_server_auth_is_configured();

    if (strcmp(action, "setup") == 0) {
        if (configured) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "{\"error\":\"already configured\"}");
        }
        if (auth_password_hash(password, computed) != ESP_OK ||
                auth_write_hash(computed) != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_500(req);
        }

        /* 首次设置时顺带保存 Wi-Fi，否则新设备连上热点后无处配网 */
        const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid"));
        if (ssid && ssid[0]) {
            http_server_ctx_t *ctx = http_server_ctx();
            app_config_t *config = calloc(1, sizeof(*config));

            if (config && ctx->services.load_config(config) == ESP_OK) {
                const char *wifi_pass =
                    cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "wifi_password"));

                strlcpy(config->wifi_ssid, ssid, sizeof(config->wifi_ssid));
                strlcpy(config->wifi_password, wifi_pass ? wifi_pass : "",
                        sizeof(config->wifi_password));
                ctx->services.save_config(config);
            }
            free(config);
        }
    } else if (strcmp(action, "change") == 0) {
        const char *new_password =
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "new_password"));
        size_t new_len = new_password ? strlen(new_password) : 0;

        if (new_len < AUTH_PASSWORD_MIN || new_len > AUTH_PASSWORD_MAX) {
            cJSON_Delete(root);
            return auth_bad_request(req, "new_password length must be 6..64");
        }
        if (!configured || auth_read_hash(stored) != ESP_OK ||
                auth_password_hash(password, computed) != ESP_OK ||
                !auth_streq_const(stored, computed)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "401 Unauthorized");
            return httpd_resp_sendstr(req, "{\"error\":\"wrong password\"}");
        }
        if (auth_password_hash(new_password, computed) != ESP_OK ||
                auth_write_hash(computed) != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_500(req);
        }
        /* 旧 token 立即作废，下面统一签发新的 */
        s_token_valid = false;
    } else if (strcmp(action, "login") == 0) {
        if (!configured || auth_read_hash(stored) != ESP_OK ||
                auth_password_hash(password, computed) != ESP_OK ||
                !auth_streq_const(stored, computed)) {
            cJSON_Delete(root);
            ESP_LOGW(TAG, "failed login attempt");
            httpd_resp_set_status(req, "401 Unauthorized");
            return httpd_resp_sendstr(req, "{\"error\":\"wrong password\"}");
        }
    } else {
        cJSON_Delete(root);
        return auth_bad_request(req, "unknown action");
    }

    cJSON_Delete(root);
    cJSON *out = cJSON_CreateObject();
    if (!out) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    http_server_json_add_string(out, "token", auth_issue_token());
    ESP_LOGI(TAG, "auth %s succeeded", action);
    return http_server_send_json_response(req, out);
}

esp_err_t http_server_register_auth_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/auth", .method = HTTP_GET, .handler = auth_get_handler },
        { .uri = "/api/auth", .method = HTTP_POST, .handler = auth_post_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);

        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
