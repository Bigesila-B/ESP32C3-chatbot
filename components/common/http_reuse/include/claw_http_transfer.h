/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_http_client.h"

/* Run a synchronous request from the chat, search or QQ transport. */
esp_err_t claw_http_perform(esp_http_client_handle_t client);
