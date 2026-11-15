/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the MCP client capability group with the claw_cap registry.
 *
 * Registers group id `cap_mcp_client` with callable descriptors `mcp_list_servers`, `mcp_list_tools`,
 * `mcp_call_tool`, `mcp_discover`, and `mcp_last_image`.
 * Safe to call repeatedly: if the group already exists, returns `ESP_OK`
 * without registering again.
 * This only registers capability metadata and callbacks; remote MCP requests are
 * made later when one of those capabilities is invoked.
 *
 * @return ESP_OK on success, or an error code from `claw_cap_register_group()`.
 */
esp_err_t cap_mcp_client_register_group(void);

/** Validate a nonempty HTTP(S) MCP service URL. */
esp_err_t cap_mcp_client_validate_url(const char *url);

#define CAP_MCP_BEARER_MAX_LEN 319
#define CAP_MCP_URL_MAX_LEN 319
#define CAP_MCP_NAME_MAX_LEN 48
#define CAP_MCP_SERVER_LIMIT 4

typedef struct {
    uint32_t id;
    char name[CAP_MCP_NAME_MAX_LEN + 1];
    char url[CAP_MCP_URL_MAX_LEN + 1];
    char token[CAP_MCP_BEARER_MAX_LEN + 1];
} cap_mcp_server_config_t;

typedef struct {
    size_t count;
    uint32_t version;
    cap_mcp_server_config_t items[CAP_MCP_SERVER_LIMIT];
} cap_mcp_servers_t;

/** Validate a raw Bearer token. An empty string disables authentication. */
esp_err_t cap_mcp_client_validate_bearer_token(const char *token);

/** Validate or replace the named catalog. Its first entry is the default. */
esp_err_t cap_mcp_client_validate_servers(const cap_mcp_servers_t *servers);
esp_err_t cap_mcp_client_set_servers(const cap_mcp_servers_t *servers);
void cap_mcp_client_get_servers(cap_mcp_servers_t *servers);

/** Return true once a named catalog is installed, even when it is empty. */
bool cap_mcp_client_get_named_default(char *url, char *token, size_t size);

/** Set the legacy default URL and credential before a named catalog is installed. */
esp_err_t cap_mcp_client_set_server(const char *url, const char *bearer_token);

/** Set the legacy default URL. An empty string clears it. */
esp_err_t cap_mcp_client_set_server_url(const char *url);

/** List tools using the supplied or default server_url. The caller owns the JSON result. */
esp_err_t cap_mcp_client_list_tools(const cJSON *input, cJSON **result_out);

/** Test a settings URL. NULL reuses the saved token for the same URL only. */
esp_err_t cap_mcp_client_test_server(const char *url, const char *bearer_token, cJSON **result_out);
esp_err_t cap_mcp_client_test_server_entry(const char *url, const char *bearer_token,
                                           uint32_t server_id, cJSON **result_out);

/** Set the app's chat delivery callback during startup. */
typedef esp_err_t (*cap_mcp_chat_sender_t)(const char *channel, const char *chat_id,
                                          const char *content, bool image);
void cap_mcp_client_set_chat_sender(cap_mcp_chat_sender_t sender);

/** Send this chat's latest generated image, refreshing a job's download URL if needed. */
esp_err_t cap_mcp_client_resend_image(const char *channel, const char *chat_id);

/** A cached inline result from an MCP tool. Keep the handle until the response finishes. */
typedef struct cap_mcp_image cap_mcp_image_t;
cap_mcp_image_t *cap_mcp_client_image_acquire(uint32_t id);
const void *cap_mcp_client_image_data(const cap_mcp_image_t *image, size_t *size, const char **mime);
void cap_mcp_client_image_release(cap_mcp_image_t *image);

#ifdef __cplusplus
}
#endif
