/* SPDX-License-Identifier: Apache-2.0 */
#include "claw_http_transfer.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32C3 && CONFIG_APP_CLAW_TEXT_ONLY
#include <sys/lock.h>
static _lock_t transfer_lock;
#endif

esp_err_t claw_http_perform(esp_http_client_handle_t client)
{
#if CONFIG_IDF_TARGET_ESP32C3 && CONFIG_APP_CLAW_TEXT_ONLY
    /* Tool progress and QQ reconnects can start on other tasks while a search
     * is running. Allow only one transient TLS handshake at a time. The QQ
     * WebSocket and its heartbeat run independently of this lock. */
    _lock_acquire(&transfer_lock);
    esp_err_t err = esp_http_client_perform(client);
    _lock_release(&transfer_lock);
    return err;
#else
    return esp_http_client_perform(client);
#endif
}
