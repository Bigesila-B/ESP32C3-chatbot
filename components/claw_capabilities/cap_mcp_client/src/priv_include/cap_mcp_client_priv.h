/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_err.h"
#include "mcp_mdns.h"

#define MCP_IMAGE_FIELD_MAX 128

/**
 * @file cap_mcp_client_priv.h
 * @brief Internal helpers used by the cap_mcp_client capability callbacks.
 *
 * Internal MCP tool invocation and mDNS discovery helpers.
 */

typedef struct {
    const char *id;
    const char *status;
    const char *images;
} cap_mcp_image_fields_t;

typedef struct {
    cap_mcp_image_fields_t fields;
    const char *status_tool;
    const char *id_argument;
    const char *request_id_argument;
} cap_mcp_image_delivery_t;

typedef struct {
    const cap_mcp_image_delivery_t *delivery;
    uint32_t turn_id;
    uint32_t server_revision;
    bool can_poll_image;
} cap_mcp_call_options_t;

/**
 * Call remote MCP `tools/call` over HTTP. NULL options use the ordinary call path.
 * A nonzero server_revision pins follow-up calls to their original connection.
 * The caller owns the returned JSON result.
 */
esp_err_t cap_mcp_call_remote_tool(const cJSON *input, cap_mcp_call_options_t *options,
                                  cJSON **result_out);

typedef enum {
    MCP_IMAGE_NO_STATUS,
    MCP_IMAGE_PENDING,
    MCP_IMAGE_READY,
    MCP_IMAGE_FAILED,
} cap_mcp_image_status_t;

#define MCP_RESULT_IMAGE_MAX 4

typedef struct {
    char *urls[MCP_RESULT_IMAGE_MAX];
    size_t count;
} cap_mcp_image_links_t;

typedef struct {
    cJSON *job_id;
    cap_mcp_image_links_t links;
    cap_mcp_image_status_t status;
} cap_mcp_image_result_t;

/** Convert a tool result to text and lightweight image references for the agent. */
void cap_mcp_format_result(const cJSON *result, const claw_cap_call_context_t *ctx,
                           char *output, size_t size);

/** Extract generated image links and job status. The caller owns the links and job ID. */
void cap_mcp_read_image(const cJSON *result, bool web, const cap_mcp_image_fields_t *fields,
                         cap_mcp_image_result_t *image);

/** Handle a generation result and report whether the firmware took over delivery. */
bool cap_mcp_deliver_image(const cJSON *input, const cJSON *result,
                           const cap_mcp_call_options_t *options,
                           const claw_cap_call_context_t *ctx, char *output, size_t size);

/** Parse a bounded remote JSON document, limiting recursion on the device stack. */
cJSON *cap_mcp_parse_json(const char *text, size_t length);

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
