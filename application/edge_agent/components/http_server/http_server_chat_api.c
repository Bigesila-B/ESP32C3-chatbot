/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "app_claw.h"
#include "claw_core.h"
#include "esp_log.h"
#include "http_server_priv.h"

static const char *TAG = "http_chat";

/* Web 对话默认落在 web 会话，前端可指定其它会话以支持聊天记录切换 */
#define HTTP_CHAT_SESSION_ID   "web"
#define HTTP_CHAT_SESSION_MAX  32
#define HTTP_CHAT_CHANNEL      "web"
#define HTTP_CHAT_SENDER       "web_user"
#define HTTP_CHAT_SUBMIT_MS    5000
/* 模型可能连续调用多次工具，给足等待时间 */
#define HTTP_CHAT_REPLY_MS     90000
#define HTTP_CHAT_MAX_INPUT    2000

/* 会话 id 与 claw_memory 的命名规则保持一致：字母数字、下划线、短横线 */
static bool chat_session_id_valid(const char *id)
{
    size_t len;

    if (!id || !id[0]) {
        return false;
    }
    len = strlen(id);
    if (len > HTTP_CHAT_SESSION_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)id[i];

        if (!isalnum(ch) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

static esp_err_t chat_bad_request(httpd_req_t *req, const char *message)
{
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message);
}

static esp_err_t chat_handler(httpd_req_t *req)
{
    claw_core_handle_t core = app_claw_get_core();
    if (!core) {
        ESP_LOGE(TAG, "core not ready");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Agent not ready");
    }

    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK || !root) {
        return chat_bad_request(req, "Invalid JSON body");
    }

    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "message"));
    if (!text || !text[0]) {
        cJSON_Delete(root);
        return chat_bad_request(req, "message is required");
    }
    if (strlen(text) > HTTP_CHAT_MAX_INPUT) {
        cJSON_Delete(root);
        return chat_bad_request(req, "message is too long");
    }

    /* session 可选：不传时沿用默认的 web 会话，传了就落到指定会话 */
    char session_id[HTTP_CHAT_SESSION_MAX + 1];
    const char *want_session = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "session"));

    if (want_session && want_session[0]) {
        if (!chat_session_id_valid(want_session)) {
            cJSON_Delete(root);
            return chat_bad_request(req, "invalid session id");
        }
        strlcpy(session_id, want_session, sizeof(session_id));
    } else {
        strlcpy(session_id, HTTP_CHAT_SESSION_ID, sizeof(session_id));
    }

    /* 请求结构里的字符串只在 submit 期间被读取，submit 内部会复制 */
    claw_core_request_t creq = {0};
    creq.flags = 0;
    creq.session_id = session_id;
    creq.user_text = text;
    creq.source_channel = HTTP_CHAT_CHANNEL;
    creq.source_chat_id = HTTP_CHAT_CHANNEL;
    creq.source_sender_id = HTTP_CHAT_SENDER;
    /* 不设 target_channel：回复只进响应队列，不尝试推送到 IM */

    esp_err_t err = claw_core_submit(core, &creq, HTTP_CHAT_SUBMIT_MS);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "submit failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to submit message");
    }

    claw_core_response_t resp = {0};
    err = claw_core_receive(core, &resp, HTTP_CHAT_REPLY_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "receive failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Model reply timeout");
    }

    cJSON *out = cJSON_CreateObject();
    if (!out) {
        claw_core_response_free(&resp);
        return httpd_resp_send_500(req);
    }

    bool ok = resp.status == CLAW_CORE_RESPONSE_STATUS_OK;
    cJSON_AddBoolToObject(out, "ok", ok);
    if (ok && resp.text) {
        cJSON_AddStringToObject(out, "reply", resp.text);
    } else {
        /* 失败时把后端错误带回去，前端直接展示，便于定位 */
        cJSON_AddStringToObject(out, "reply", "");
        cJSON_AddStringToObject(out, "error", resp.error_message ? resp.error_message : "model returned an error");
    }

    claw_core_response_free(&resp);
    return http_server_send_json_response(req, out);
}

esp_err_t http_server_register_chat_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/chat", .method = HTTP_POST, .handler = chat_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
