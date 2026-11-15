/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#if CONFIG_APP_CLAW_CAP_SKILL_MGR
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "claw_cap.h"
#include "claw_skill.h"

#define SKILL_UPLOAD_PREFIX "/.skill-import-"
#define SKILL_UPLOAD_ID_LEN 16
#define SKILL_INSTALL_ROOT "/skills"

typedef struct {
    char staging[HTTP_SERVER_PATH_MAX];
    char source[HTTP_SERVER_PATH_MAX];
    char target[HTTP_SERVER_PATH_MAX];
} skill_install_paths_t;

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

static esp_err_t skill_install_from_staging(const char *upload_id, const char *skill_id, skill_install_paths_t *paths)
{
    char staging_rel[sizeof(SKILL_UPLOAD_PREFIX) + SKILL_UPLOAD_ID_LEN];
    snprintf(staging_rel, sizeof(staging_rel), SKILL_UPLOAD_PREFIX "%s", upload_id);
    if (http_server_resolve_storage_path(staging_rel, paths->staging, sizeof(paths->staging)) != ESP_OK ||
            http_server_resolve_storage_path(SKILL_INSTALL_ROOT, paths->target, sizeof(paths->target)) != ESP_OK ||
            snprintf(paths->source, sizeof(paths->source), "%s/%s/SKILL.md", paths->staging, skill_id) >= (int)sizeof(paths->source)) {
        return ESP_ERR_INVALID_SIZE;
    }
    struct stat st;
    if (stat(paths->source, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *strrchr(paths->source, '/') = '\0';

    if (mkdir(paths->target, 0755) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    if (strlcat(paths->target, "/", sizeof(paths->target)) >= sizeof(paths->target) ||
            strlcat(paths->target, skill_id, sizeof(paths->target)) >= sizeof(paths->target)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (stat(paths->target, &st) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (errno != ENOENT) {
        return ESP_FAIL;
    }
    esp_err_t err = claw_skill_validate_install(paths->staging, skill_id);
    if (err != ESP_OK) {
        return err;
    }

    if (rename(paths->source, paths->target) != 0) {
        return errno == EEXIST ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    (void)rmdir(paths->staging);
    return ESP_OK;
}

static esp_err_t skill_catalog_install_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "skill_invalid");
    }
    const char *upload_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "upload_id"));
    const char *skill_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "skill_id"));
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (upload_id && strspn(upload_id, "0123456789abcdef") == SKILL_UPLOAD_ID_LEN &&
            upload_id[SKILL_UPLOAD_ID_LEN] == '\0' && skill_id && *skill_id &&
            skill_id[0] != ' ' && skill_id[strlen(skill_id) - 1] != ' ' && skill_id[strlen(skill_id) - 1] != '.' &&
            !strstr(skill_id, "..") && !strpbrk(skill_id, "/\\:\"<>|?*")) {
        skill_install_paths_t *paths = malloc(sizeof(*paths));
        err = paths ? skill_install_from_staging(upload_id, skill_id, paths) : ESP_ERR_NO_MEM;
        free(paths);
    }
    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "skill_exists");
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "skill_invalid");
    }
    return httpd_resp_send_500(req);
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
    if (!cJSON_AddBoolToObject(root, "can_upload", true)) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    esp_err_t err = claw_skill_foreach_catalog_entry(catalog_add_skill, skills);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        if (err == ESP_ERR_INVALID_STATE) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_sendstr(req, "The skill catalog is not ready.");
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
    esp_err_t err = httpd_register_uri_handler(server, &route);
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
    if (err == ESP_OK) {
        const httpd_uri_t install = {
            .uri = "/api/skills",
            .method = HTTP_POST,
            .handler = skill_catalog_install_handler,
        };
        err = httpd_register_uri_handler(server, &install);
    }
#endif
    return err;
}
