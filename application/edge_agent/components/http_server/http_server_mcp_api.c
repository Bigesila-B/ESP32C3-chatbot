/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdlib.h>

#include "cap_mcp_client.h"
#include "claw_cap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MCP_TEST_STACK_SIZE    8192
#define MCP_TEST_MAX_NAMES     24

typedef struct {
    httpd_req_t *req;
    cJSON *input;
} mcp_test_request_t;

static portMUX_TYPE s_test_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_test_running;

static void mcp_test_release(void)
{
    portENTER_CRITICAL(&s_test_lock);
    s_test_running = false;
    portEXIT_CRITICAL(&s_test_lock);
}

static esp_err_t mcp_test_send_result(httpd_req_t *req, cJSON *result)
{
    const char *error = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "error_message"));
    const cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    if (error || !cJSON_IsArray(tools)) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req, error ? error : "Invalid MCP tools/list response");
    }

    cJSON *response = cJSON_CreateObject();
    cJSON *names = cJSON_AddArrayToObject(response, "names");
    if (!names) {
        cJSON_Delete(response);
        return httpd_resp_send_500(req);
    }
    const cJSON *tool;
    cJSON_ArrayForEach(tool, tools) {
        if (cJSON_GetArraySize(names) == MCP_TEST_MAX_NAMES) {
            break;
        }
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(tool, "name"));
        cJSON *item = name ? cJSON_CreateString(name) : NULL;
        if (item && !cJSON_AddItemToArray(names, item)) {
            cJSON_Delete(item);
        }
    }
    const char *cursor = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "nextCursor"));
    cJSON_AddNumberToObject(response, "count", cJSON_GetArraySize(tools));
    cJSON_AddBoolToObject(response, "has_more", cursor && cursor[0]);
    return http_server_send_json_response(req, response);
}

static void mcp_test_task(void *arg)
{
    mcp_test_request_t *test = arg;
    cJSON *result = NULL;
    esp_err_t err = cap_mcp_client_list_tools(test->input, &result);
    if (err == ESP_OK) {
        mcp_test_send_result(test->req, result);
    } else {
        httpd_resp_set_status(test->req, "502 Bad Gateway");
        httpd_resp_sendstr(test->req, esp_err_to_name(err));
    }

    cJSON_Delete(result);
    cJSON_Delete(test->input);
    httpd_req_async_handler_complete(test->req);
    free(test);
    mcp_test_release();
    vTaskDelete(NULL);
}

static esp_err_t mcp_test_post_handler(httpd_req_t *req)
{
    claw_cap_state_t state;
    if (claw_cap_get_group_state("cap_mcp_client", &state) != ESP_OK || state != CLAW_CAP_STATE_STARTED) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "MCP Client is disabled");
    }

    cJSON *input = NULL;
    if (http_server_parse_json_body(req, &input) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "server_url"));
    if (cap_mcp_client_validate_url(url) != ESP_OK) {
        cJSON_Delete(input);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid MCP server URL");
    }
    /* The Web test accepts one complete URL, just like the saved setting. */
    cJSON_DeleteItemFromObjectCaseSensitive(input, "endpoint");
    cJSON_DeleteItemFromObjectCaseSensitive(input, "cursor");

    portENTER_CRITICAL(&s_test_lock);
    bool busy = s_test_running;
    s_test_running = true;
    portEXIT_CRITICAL(&s_test_lock);
    if (busy) {
        cJSON_Delete(input);
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "An MCP test is already running");
    }

    mcp_test_request_t *test = calloc(1, sizeof(*test));
    if (!test || httpd_req_async_handler_begin(req, &test->req) != ESP_OK) {
        free(test);
        cJSON_Delete(input);
        mcp_test_release();
        return httpd_resp_send_500(req);
    }
    test->input = input;
    if (xTaskCreate(mcp_test_task, "mcp_test", MCP_TEST_STACK_SIZE, test,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        httpd_resp_send_500(test->req);
        httpd_req_async_handler_complete(test->req);
        cJSON_Delete(input);
        free(test);
        mcp_test_release();
    }
    return ESP_OK;
}

esp_err_t http_server_register_mcp_routes(httpd_handle_t server)
{
    const httpd_uri_t route = {
        .uri = "/api/mcp/test",
        .method = HTTP_POST,
        .handler = mcp_test_post_handler,
    };
    return httpd_register_uri_handler(server, &route);
}
