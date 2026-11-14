/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "mcp_mdns.h"

/**
 * @file cap_mcp_client_priv.h
 * @brief Internal helpers used by the cap_mcp_client capability callbacks.
 *
 * Internal MCP tool invocation and mDNS discovery helpers.
 */

/**
 * @brief Call remote MCP `tools/call` over HTTP via mcp-c-sdk (`esp_mcp_mgr_post_tools_call`).
 *
 * @param input JSON object with required "tool_name" and optional "server_url",
 *              "endpoint" and "arguments". Omit "server_url" to use the saved URL.
 * @param result_out On success, receives a heap-allocated `cJSON` object with the parsed RPC
 *                   result (caller must free with `cJSON_Delete`). Set to NULL on entry.
 *
 * @return ESP_OK on success; `ESP_ERR_INVALID_ARG` / `ESP_ERR_NO_MEM` on parse or allocation
 *         failure; other codes from the MCP manager or transport on failure.
 */
esp_err_t cap_mcp_call_remote_tool(const cJSON *input, cJSON **result_out);

/**
 * @brief Discover MCP HTTP services on the LAN via mDNS (`mcp_mdns_query_devices`).
 *
 * Optionally merges in the local device when the onboard MCP server is running (see
 * `mcp_mdns_get_config`).
 *
 * @param input_json Optional; NULL or empty uses defaults. Otherwise a JSON object with
 *                   optional `"timeout_ms"` (positive int) and `"include_self"` (bool).
 * @param result_out On success, receives a heap-allocated `cJSON` summary (typically
 *                   `count` and `devices`; caller must free with `cJSON_Delete`).
 *                   Set to NULL on entry.
 *
 * @return ESP_OK on success; `ESP_ERR_INVALID_ARG` if `result_out` is NULL or JSON is invalid.
 */
esp_err_t cap_mcp_discover_services(const char *input_json, cJSON **result_out);
