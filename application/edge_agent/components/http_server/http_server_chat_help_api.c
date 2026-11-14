/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include "app_chat_help.h"

static esp_err_t chat_help_get_handler(httpd_req_t *req)
{
    cJSON *root = app_chat_help_json();
    return root ? http_server_send_json_response(req, root) : httpd_resp_send_500(req);
}

esp_err_t http_server_register_chat_help_routes(httpd_handle_t server)
{
    const httpd_uri_t route = {
        .uri = "/api/chat/help",
        .method = HTTP_GET,
        .handler = chat_help_get_handler,
    };
    return httpd_register_uri_handler(server, &route);
}
