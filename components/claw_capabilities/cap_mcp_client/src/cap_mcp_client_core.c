/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_http_client.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_mgr.h"
#include "freertos/FreeRTOS.h"
#include "http_parser.h"

#include "cap_mcp_client.h"
#include "cap_mcp_client_priv.h"

#define CAP_MCP_HTTP_TIMEOUT_MS       20000
#define CAP_MCP_END_SESSION_TIMEOUT   2000
#define CAP_MCP_SERVER_URL_LEN        256

typedef struct {
    esp_mcp_t *engine;
    esp_mcp_mgr_handle_t manager;
    char *endpoint;
    char *url;
    size_t origin_len;
    char *session_id;
    char protocol_version[32];
} cap_mcp_session_t;

typedef struct {
    cJSON *result;
    esp_err_t err;
    cap_mcp_session_t *initializing_session;
} cap_mcp_response_ctx_t;

typedef esp_err_t (*cap_mcp_post_fn)(esp_mcp_mgr_handle_t, const esp_mcp_mgr_req_t *);

static portMUX_TYPE s_server_url_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_server_url[CAP_MCP_SERVER_URL_LEN];
/* SDK callbacks run synchronously on the task that owns this session. */
static _Thread_local bool s_http_request_active;
static _Thread_local bool s_initialized_deferred;

static esp_err_t cap_mcp_http_request(esp_mcp_transport_handle_t handle,
                                     const uint8_t *data, uint16_t size)
{
    if (s_http_request_active) {
        /* The SDK sends this from the response callback. The HTTP handle is
         * still processing initialize, so send it after that POST returns. */
        if (strstr((const char *)data, "\"method\":\"notifications/initialized\"")) {
            s_initialized_deferred = true;
            return ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;
    }

    s_http_request_active = true;
    esp_err_t err = esp_mcp_transport_http_client.request(handle, data, size);
    s_http_request_active = false;
    return err;
}

static esp_err_t cap_mcp_parse_url(const char *url, struct http_parser_url *parsed)
{
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) ||
            strlen(url) >= CAP_MCP_SERVER_URL_LEN || strpbrk(url, "# \t\r\n")) {
        return ESP_ERR_INVALID_ARG;
    }

    http_parser_url_init(parsed);
    if (http_parser_parse_url(url, strlen(url), 0, parsed) != 0 ||
            !(parsed->field_set & (1 << UF_HOST)) ||
            (parsed->field_set & (1 << UF_USERINFO))) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t cap_mcp_client_validate_url(const char *url)
{
    struct http_parser_url parsed;
    return cap_mcp_parse_url(url, &parsed);
}

esp_err_t cap_mcp_client_set_server_url(const char *url)
{
    if (!url || (url[0] && cap_mcp_client_validate_url(url) != ESP_OK)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_server_url_lock);
    strlcpy(s_server_url, url, sizeof(s_server_url));
    portEXIT_CRITICAL(&s_server_url_lock);
    return ESP_OK;
}

static esp_err_t cap_mcp_mgr_response_cb(int error_code,
                                        const char *ep_name,
                                        const char *resp_json,
                                        void *user_ctx,
                                        uint32_t jsonrpc_request_id)
{
    (void)ep_name;
    (void)jsonrpc_request_id;

    cap_mcp_response_ctx_t *ctx = user_ctx;
    cJSON_Delete(ctx->result);
    ctx->result = NULL;
    ctx->err = ESP_OK;

    /* The SDK passes isError (0 or 1) for a JSON-RPC result. */
    if (error_code == 0 || error_code == 1) {
        ctx->result = cJSON_Parse(resp_json);
        if (!cJSON_IsObject(ctx->result)) {
            ctx->err = ESP_ERR_INVALID_RESPONSE;
        } else if (ctx->initializing_session) {
            cap_mcp_session_t *session = ctx->initializing_session;
            const char *session_id = esp_mcp_get_request_session_id(session->engine);
            if (session_id) {
                free(session->session_id);
                session->session_id = strdup(session_id);
            }
            const char *protocol = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ctx->result,
                                                                                        "protocolVersion"));
            if (protocol) {
                strlcpy(session->protocol_version, protocol, sizeof(session->protocol_version));
            }
        }
    } else if (error_code < 0) {
        ctx->result = cJSON_CreateObject();
        if (!cJSON_AddStringToObject(ctx->result, "error_message",
                                     resp_json ? resp_json : "MCP protocol error")) {
            ctx->err = ESP_ERR_NO_MEM;
        }
    } else {
        ctx->err = error_code;
    }
    return ctx->err;
}

static void cap_mcp_session_end_remote(const cap_mcp_session_t *session)
{
    if (!session->session_id || !session->protocol_version[0]) {
        return;
    }
    size_t size = session->origin_len + strlen(session->endpoint) + 2;
    char *url = malloc(size);
    if (!url) {
        return;
    }
    memcpy(url, session->url, session->origin_len);
    url[session->origin_len] = '\0';
    if (session->endpoint[0] != '/') {
        strlcat(url, "/", size);
    }
    strlcat(url, session->endpoint, size);

    /* The SDK's DELETE reuses the already freed body of the previous POST. */
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_DELETE,
        .timeout_ms = CAP_MCP_END_SESSION_TIMEOUT,
        .disable_auto_redirect = true,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (http) {
        if (esp_http_client_set_header(http, "Mcp-Session-Id", session->session_id) == ESP_OK &&
                esp_http_client_set_header(http, "MCP-Protocol-Version", session->protocol_version) == ESP_OK) {
            (void)esp_http_client_perform(http);
        }
        esp_http_client_cleanup(http);
    }
    free(url);
}

static void cap_mcp_session_close(cap_mcp_session_t *session)
{
    if (session->manager) {
        esp_mcp_mgr_stop(session->manager);
        esp_mcp_mgr_deinit(session->manager);
    }
    if (session->engine) {
        esp_mcp_destroy(session->engine);
    }
    cap_mcp_session_end_remote(session);
    free(session->session_id);
    free(session->url);
    free(session->endpoint);
}

static esp_err_t cap_mcp_session_start(cap_mcp_session_t *session)
{
    esp_err_t err = esp_mcp_create(&session->engine);
    if (err != ESP_OK) {
        return err;
    }

    esp_http_client_config_t http_config = {
        .url = session->url,
        .timeout_ms = CAP_MCP_HTTP_TIMEOUT_MS,
    };
    esp_mcp_mgr_config_t mgr_config = {
        .transport = esp_mcp_transport_http_client,
        .config = &http_config,
        .instance = session->engine,
    };
    mgr_config.transport.request = cap_mcp_http_request;

    err = esp_mcp_mgr_init(mgr_config, &session->manager);
    if (err == ESP_OK) {
        err = esp_mcp_mgr_start(session->manager);
    }
    if (err == ESP_OK) {
        err = esp_mcp_mgr_register_endpoint(session->manager, session->endpoint, NULL);
    }
    return err;
}

static esp_err_t cap_mcp_session_open(cap_mcp_session_t *session, const cJSON *input)
{
    const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(input, "server_url");
    if (url_item) {
        const char *url = cJSON_GetStringValue(url_item);
        if (!url || strlen(url) >= CAP_MCP_SERVER_URL_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        session->url = strdup(url);
    } else {
        session->url = malloc(CAP_MCP_SERVER_URL_LEN);
        if (!session->url) {
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_server_url_lock);
        strlcpy(session->url, s_server_url, CAP_MCP_SERVER_URL_LEN);
        portEXIT_CRITICAL(&s_server_url_lock);
        if (!session->url[0]) {
            return ESP_ERR_NOT_FOUND;
        }
    }
    if (!session->url) {
        return ESP_ERR_NO_MEM;
    }

    const char *url = session->url;
    struct http_parser_url parsed;
    esp_err_t err = cap_mcp_parse_url(url, &parsed);
    if (err != ESP_OK) {
        return err;
    }
    session->origin_len = strcspn(url + parsed.field_data[UF_HOST].off, "/?") + parsed.field_data[UF_HOST].off;

    const char *endpoint = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "endpoint"));
    if (endpoint && endpoint[0]) {
        if (strlen(endpoint) >= CAP_MCP_SERVER_URL_LEN || strpbrk(endpoint, "# \t\r\n")) {
            return ESP_ERR_INVALID_ARG;
        }
        session->endpoint = strdup(endpoint);
    } else if (parsed.field_set & (1 << UF_PATH)) {
        session->endpoint = strdup(url + parsed.field_data[UF_PATH].off);
    } else {
        const char *query = strchr(url, '?');
        size_t size = sizeof(MCP_MDNS_DEFAULT_ENDPOINT) + (query ? strlen(query) : 0);
        session->endpoint = malloc(size);
        if (session->endpoint) {
            strlcpy(session->endpoint, MCP_MDNS_DEFAULT_ENDPOINT, size);
            strlcat(session->endpoint, query ? query : "", size);
        }
    }

    if (!session->endpoint) {
        return ESP_ERR_NO_MEM;
    }
    err = cap_mcp_session_start(session);
    if (err != ESP_OK) {
        return err;
    }

    cap_mcp_response_ctx_t ctx = { .initializing_session = session };
    esp_mcp_mgr_req_t req = {
        .ep_name = session->endpoint,
        .cb = cap_mcp_mgr_response_cb,
        .user_ctx = &ctx,
        .u.init = {
            .name = "esp-claw",
            .version = "1.0.0",
            .title = "ESP-Claw",
        },
    };
    s_initialized_deferred = false;
    err = esp_mcp_mgr_post_info_init(session->manager, &req);
    if (err == ESP_OK) {
        err = ctx.err;
        if (err == ESP_OK && !session->protocol_version[0]) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    cJSON_Delete(ctx.result);
    if (err == ESP_OK && s_initialized_deferred) {
        err = esp_mcp_mgr_post_initialized(session->manager, session->endpoint);
    }
    return err;
}

static esp_err_t cap_mcp_request(const cJSON *input, esp_mcp_mgr_req_t *req,
                                 cap_mcp_post_fn post, cJSON **result_out)
{
    if (!result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_out = NULL;
    if (!cJSON_IsObject(input)) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_mcp_session_t session = {0};
    cap_mcp_response_ctx_t ctx = {0};
    esp_err_t err = cap_mcp_session_open(&session, input);
    if (err == ESP_OK) {
        req->ep_name = session.endpoint;
        req->cb = cap_mcp_mgr_response_cb;
        req->user_ctx = &ctx;
        err = post(session.manager, req);
    }
    cap_mcp_session_close(&session);

    if (err == ESP_OK) {
        err = ctx.err;
        if (err == ESP_OK && !ctx.result) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (err == ESP_OK) {
        *result_out = ctx.result;
    } else {
        cJSON_Delete(ctx.result);
    }
    return err;
}

esp_err_t cap_mcp_client_list_tools(const cJSON *input, cJSON **result_out)
{
    const char *cursor = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "cursor"));
    esp_mcp_mgr_req_t req = {
        .u.list = {
            .cursor = cursor,
            .limit = -1,
        },
    };
    return cap_mcp_request(input, &req, esp_mcp_mgr_post_tools_list, result_out);
}

esp_err_t cap_mcp_call_remote_tool(const cJSON *input, cJSON **result_out)
{
    const char *tool_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "tool_name"));
    if (!tool_name || !tool_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(input, "arguments");
    char *arguments_json = NULL;
    if (cJSON_IsObject(arguments)) {
        arguments_json = cJSON_PrintUnformatted(arguments);
        if (!arguments_json) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_mcp_mgr_req_t req = {
        .u.call = {
            .tool_name = tool_name,
            .args_json = arguments_json ? arguments_json : "{}",
        },
    };
    esp_err_t err = cap_mcp_request(input, &req, esp_mcp_mgr_post_tools_call, result_out);
    cJSON_free(arguments_json);
    return err;
}
