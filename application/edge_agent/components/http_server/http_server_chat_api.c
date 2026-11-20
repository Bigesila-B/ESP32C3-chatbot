/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "app_claw.h"
#include "claw_core.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server_priv.h"
#include "lwip/sockets.h"

static const char *TAG = "http_chat";

/* Web 对话默认落在 web 会话，前端可指定其它会话以支持聊天记录切换 */
#define HTTP_CHAT_SESSION_ID   "web"
#define HTTP_CHAT_SESSION_MAX  32
#define HTTP_CHAT_CHANNEL      "web"
#define HTTP_CHAT_SENDER       "web_user"
#define HTTP_CHAT_SUBMIT_MS    5000
/* 模型可能连续调用多次工具，给足等待时间。
 * 必须大于模型请求自身的超时（默认 120 s），否则这里先返回、LLM 还在后台跑，
 * 占用标记提前释放，后续请求就会和它叠在一起把堆吃光。 */
#define HTTP_CHAT_REPLY_MS     130000
#define HTTP_CHAT_MAX_INPUT    2000

/* C3 只有几十 KB 堆，一次对话在模型返回前会一直占着 TLS 缓冲和请求上下文。
 * 上一轮还没收尾时又进来一条，就会把堆吃光，之后连 QQ 通道都握手失败。
 * 所以同一时刻只放一个请求进主链路，其余直接返回 409。 */
#define HTTP_CHAT_WORKER_STACK 4096

static atomic_bool s_chat_busy;
static atomic_bool s_chat_stopping;

typedef struct {
    httpd_req_t *req;
    cJSON *body;
} http_chat_job_t;

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

static esp_err_t chat_busy_response(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();

    if (!out) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", false);
    cJSON_AddStringToObject(out, "reply", "");
    cJSON_AddStringToObject(out, "error", "上一条消息还在处理中，等几秒再发");
    httpd_resp_set_status(req, "409 Conflict");
    return http_server_send_json_response(req, out);
}

static esp_err_t chat_dispatch(httpd_req_t *req, cJSON *root)
{
    claw_core_handle_t core = app_claw_get_core();
    if (!core) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "core not ready");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Agent not ready");
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

static void chat_worker(void *arg)
{
    http_chat_job_t *job = arg;
    httpd_req_t *req = job->req;
    cJSON *body = job->body;
    free(job);

    if (chat_dispatch(req, body) != ESP_OK) {
        shutdown(httpd_req_to_sockfd(req), SHUT_RDWR);
    }
    httpd_req_async_handler_complete(req);
    atomic_store(&s_chat_busy, false);
    vTaskDelete(NULL);
}

/* Read the POST on the server thread, then let it serve status and other
 * sockets while one worker waits for the model. */
static esp_err_t chat_handler(httpd_req_t *req)
{
    if (atomic_exchange(&s_chat_busy, true)) {
        return chat_busy_response(req);
    }
    if (atomic_load(&s_chat_stopping)) {
        atomic_store(&s_chat_busy, false);
        return chat_busy_response(req);
    }

    cJSON *body = NULL;
    if (http_server_parse_json_body(req, &body) != ESP_OK || !body) {
        atomic_store(&s_chat_busy, false);
        return chat_bad_request(req, "Invalid JSON body");
    }
    http_chat_job_t *job = malloc(sizeof(*job));
    if (job && httpd_req_async_handler_begin(req, &job->req) == ESP_OK) {
        job->body = body;
        if (xTaskCreate(chat_worker, "web_chat", HTTP_CHAT_WORKER_STACK, job,
                        tskIDLE_PRIORITY + 2, NULL) == pdPASS) {
            return ESP_OK;
        }
        httpd_req_async_handler_complete(job->req);
    }
    cJSON_Delete(body);
    free(job);
    atomic_store(&s_chat_busy, false);
    return httpd_resp_send_500(req);
}

void http_server_chat_drain(void)
{
    atomic_store(&s_chat_stopping, true);
    while (atomic_load(&s_chat_busy)) {
        vTaskDelay(1);
    }
}

esp_err_t http_server_register_chat_routes(httpd_handle_t server)
{
    atomic_store(&s_chat_stopping, false);
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
