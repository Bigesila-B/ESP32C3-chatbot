/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#if CONFIG_APP_CLAW_CAP_SKILL_MGR
#include "claw_cap.h"
#include "claw_skill.h"

static esp_err_t catalog_add_skill(const claw_skill_catalog_entry_t *entry, void *ctx)
{
    cJSON *item = cJSON_CreateObject();
    if (!item || !cJSON_AddStringToObject(item, "id", entry->id) ||
            !cJSON_AddStringToObject(item, "summary", entry->summary)) {
        cJSON_Delete(item);
        return ESP_ERR_NO_MEM;
    }
    cJSON *unavailable = cJSON_AddArrayToObject(item, "unavailable_groups");
    if (!unavailable) {
        cJSON_Delete(item);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < entry->cap_group_count; ++i) {
        claw_cap_state_t state;
        const char *group = entry->cap_groups[i];
        if (claw_cap_get_group_state(group, &state) != ESP_OK || state != CLAW_CAP_STATE_STARTED) {
            cJSON *name = cJSON_CreateString(group);
            if (!name || !cJSON_AddItemToArray(unavailable, name)) {
                cJSON_Delete(name);
                cJSON_Delete(item);
                return ESP_ERR_NO_MEM;
            }
        }
    }
    if (!cJSON_AddItemToArray(ctx, item)) {
        cJSON_Delete(item);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
#endif

static esp_err_t skill_catalog_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *skills = cJSON_AddArrayToObject(root, "skills");
    if (!skills) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
    esp_err_t err = claw_skill_foreach_catalog_entry(catalog_add_skill, skills);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        if (err == ESP_ERR_INVALID_STATE) {
            return httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, NULL);
        }
        return httpd_resp_send_500(req);
    }
#endif
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_skill_catalog_routes(httpd_handle_t server)
{
    const httpd_uri_t route = {
        .uri = "/api/skills",
        .method = HTTP_GET,
        .handler = skill_catalog_get_handler,
    };
    return httpd_register_uri_handler(server, &route);
}
