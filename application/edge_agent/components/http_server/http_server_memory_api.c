/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_memory.h"
#include "esp_log.h"
#include "http_server_priv.h"

static const char *TAG = "http_memory";

/* 网页手动写入的记忆统一标记来源，便于和模型自动提取的条目区分 */
#define HTTP_MEMORY_MANUAL_SOURCE "manual"
#define HTTP_MEMORY_ID_SIZE       40

static esp_err_t memory_bad_request(httpd_req_t *req, const char *message)
{
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message);
}

/* 把请求体里的字段读进记忆条目，缺省字段保持空串，交给底层按修补语义处理 */
static void memory_read_item(const cJSON *root, claw_memory_item_t *item)
{
    http_server_json_read_string((cJSON *)root, "id", item->id, sizeof(item->id));
    http_server_json_read_string((cJSON *)root, "source", item->source, sizeof(item->source));
    http_server_json_read_string((cJSON *)root, "content", item->content, sizeof(item->content));
    http_server_json_read_string((cJSON *)root, "tags", item->tags, sizeof(item->tags));
    http_server_json_read_string((cJSON *)root, "keywords", item->keywords, sizeof(item->keywords));
}

static esp_err_t memory_send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    return http_server_send_json_response(req, root);
}

static esp_err_t memory_list_handler(httpd_req_t *req)
{
    char *json = NULL;
    esp_err_t err = claw_memory_list(&json);
    if (err != ESP_OK || !json) {
        ESP_LOGE(TAG, "list failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read memory list");
    }

    /* claw_memory_list 输出的是 cJSON 打印的字符串，需按 cJSON 规则释放 */
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return send_err;
}

static esp_err_t memory_create_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK || !root) {
        return memory_bad_request(req, "Invalid JSON body");
    }

    claw_memory_item_t *item = calloc(1, sizeof(*item));
    if (!item) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }

    memory_read_item(root, item);
    cJSON_Delete(root);

    if (item->content[0] == '\0') {
        free(item);
        return memory_bad_request(req, "content is required");
    }
    if (item->source[0] == '\0') {
        strlcpy(item->source, HTTP_MEMORY_MANUAL_SOURCE, sizeof(item->source));
    }

    esp_err_t err = claw_memory_store(item);
    free(item);

    if (err == ESP_ERR_INVALID_ARG) {
        return memory_bad_request(req, "content is invalid or too long");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create memory");
    }
    return memory_send_ok(req);
}

static esp_err_t memory_update_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK || !root) {
        return memory_bad_request(req, "Invalid JSON body");
    }

    claw_memory_item_t *item = calloc(1, sizeof(*item));
    if (!item) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }

    memory_read_item(root, item);
    cJSON_Delete(root);

    if (item->id[0] == '\0') {
        free(item);
        return memory_bad_request(req, "id is required");
    }

    char item_id_for_log[HTTP_MEMORY_ID_SIZE];
    strlcpy(item_id_for_log, item->id, sizeof(item_id_for_log));

    esp_err_t err = claw_memory_update(item);
    free(item);

    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Memory not found");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return memory_bad_request(req, "content is invalid or too long");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update failed id=%s: %s", item_id_for_log, esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update memory");
    }
    return memory_send_ok(req);
}

static esp_err_t memory_delete_handler(httpd_req_t *req)
{
    char id[HTTP_MEMORY_ID_SIZE] = {0};
    if (http_server_query_get(req, "id", id, sizeof(id)) != ESP_OK || id[0] == '\0') {
        return memory_bad_request(req, "id is required");
    }

    esp_err_t err = claw_memory_forget(id);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Memory not found");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "delete failed id=%s: %s", id, esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete memory");
    }
    return memory_send_ok(req);
}

/* ---------- 人格与记忆文档（soul.md / identity.md / user.md） ---------- */

#define HTTP_MEMORY_DOC_MAX    8192
#define HTTP_MEMORY_PATH_MAX   128

/* 只允许这三个文件，避免通过接口读写任意路径 */
static const char *s_allowed_docs[] = { "soul.md", "identity.md", "user.md" };

static bool memory_doc_allowed(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_allowed_docs) / sizeof(s_allowed_docs[0]); i++) {
        if (strcmp(name, s_allowed_docs[i]) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t memory_doc_path(const char *name, char *out, size_t out_size)
{
    http_server_ctx_t *ctx = http_server_ctx();
    int written = snprintf(out, out_size, "%s/memory/%s", ctx->storage_base_path, name);
    return (written > 0 && (size_t)written < out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t memory_doc_get_handler(httpd_req_t *req)
{
    char name[32] = {0};
    if (http_server_query_get(req, "name", name, sizeof(name)) != ESP_OK || !memory_doc_allowed(name)) {
        return memory_bad_request(req, "name must be soul.md, identity.md or user.md");
    }

    char path[HTTP_MEMORY_PATH_MAX];
    if (memory_doc_path(name, path, sizeof(path)) != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    char *buf = calloc(1, HTTP_MEMORY_DOC_MAX);
    if (!buf) {
        return httpd_resp_send_500(req);
    }

    size_t len = 0;
    FILE *fp = fopen(path, "rb");
    if (fp) {
        len = fread(buf, 1, HTTP_MEMORY_DOC_MAX - 1, fp);
        fclose(fp);
    }
    buf[len] = '\0';
    ESP_LOGD(TAG, "read %s: %u bytes", name, (unsigned)len);

    cJSON *out = cJSON_CreateObject();
    if (!out) {
        free(buf);
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(out, "name", name);
    cJSON_AddStringToObject(out, "content", buf);
    cJSON_AddBoolToObject(out, "exists", len > 0);
    free(buf);
    return http_server_send_json_response(req, out);
}

static esp_err_t memory_doc_post_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK || !root) {
        return memory_bad_request(req, "Invalid JSON body");
    }

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "name"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "content"));

    if (!memory_doc_allowed(name) || !content) {
        cJSON_Delete(root);
        return memory_bad_request(req, "name or content invalid");
    }
    if (strlen(content) >= HTTP_MEMORY_DOC_MAX) {
        cJSON_Delete(root);
        return memory_bad_request(req, "content exceeds 8 KB");
    }

    char path[HTTP_MEMORY_PATH_MAX];
    esp_err_t err = memory_doc_path(name, path, sizeof(path));
    FILE *fp = (err == ESP_OK) ? fopen(path, "wb") : NULL;
    if (!fp) {
        ESP_LOGE(TAG, "open %s failed", path);
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open doc");
    }

    size_t len = strlen(content);
    bool written_ok = (len == 0) || (fwrite(content, 1, len, fp) == len);
    if (fclose(fp) != 0) {
        written_ok = false;
    }
    cJSON_Delete(root);

    if (!written_ok) {
        ESP_LOGE(TAG, "write %s failed", path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write doc");
    }
    ESP_LOGI(TAG, "saved %s: %u bytes", name, (unsigned)len);
    return memory_send_ok(req);
}

/* ---------- 整本文本替换记忆（每行一条） ---------- */

static esp_err_t memory_bulk_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK || !root) {
        return memory_bad_request(req, "Invalid JSON body");
    }

    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return memory_bad_request(req, "items must be an array of strings");
    }

    char *existing_json = NULL;
    cJSON *existing = NULL;
    if (claw_memory_list(&existing_json) == ESP_OK && existing_json) {
        existing = cJSON_Parse(existing_json);
    }
    cJSON_free(existing_json);

    int old_count = existing ? cJSON_GetArraySize(existing) : 0;
    int slot_count = old_count > 0 ? old_count : 1;
    bool *matched = calloc((size_t)slot_count, sizeof(bool));
    if (!matched) {
        cJSON_Delete(existing);
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }

    int added = 0;
    int kept = 0;

    const cJSON *entry;
    cJSON_ArrayForEach(entry, items) {
        const char *text = cJSON_IsString(entry) ? entry->valuestring : NULL;
        if (!text || !text[0]) {
            continue;
        }

        int hit = -1;
        for (int i = 0; i < old_count; i++) {
            if (matched[i]) {
                continue;
            }
            cJSON *row = cJSON_GetArrayItem(existing, i);
            const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "content"));
            if (content && strcmp(content, text) == 0) {
                hit = i;
                break;
            }
        }

        if (hit >= 0) {
            matched[hit] = true;
            kept++;
            continue;
        }

        /* 内容变了或新增，按新条目写入；超出单条上限的直接跳过 */
        if (strlen(text) >= sizeof(((claw_memory_item_t *)0)->content)) {
            continue;
        }
        claw_memory_item_t *item = calloc(1, sizeof(*item));
        if (!item) {
            continue;
        }
        strlcpy(item->content, text, sizeof(item->content));
        strlcpy(item->source, HTTP_MEMORY_MANUAL_SOURCE, sizeof(item->source));
        if (claw_memory_store(item) == ESP_OK) {
            added++;
        }
        free(item);
    }

    int removed = 0;
    for (int i = 0; i < old_count; i++) {
        if (matched[i]) {
            continue;
        }
        cJSON *row = cJSON_GetArrayItem(existing, i);
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "id"));
        if (id && claw_memory_forget(id) == ESP_OK) {
            removed++;
        }
    }

    free(matched);
    cJSON_Delete(existing);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "bulk sync: kept=%d added=%d removed=%d", kept, added, removed);

    cJSON *out = cJSON_CreateObject();
    if (!out) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "kept", kept);
    cJSON_AddNumberToObject(out, "added", added);
    cJSON_AddNumberToObject(out, "removed", removed);
    return http_server_send_json_response(req, out);
}

HTTP_SERVER_GATED_HANDLER(memory_list_handler)
HTTP_SERVER_GATED_HANDLER(memory_create_handler)
HTTP_SERVER_GATED_HANDLER(memory_update_handler)
HTTP_SERVER_GATED_HANDLER(memory_delete_handler)
HTTP_SERVER_GATED_HANDLER(memory_doc_get_handler)
HTTP_SERVER_GATED_HANDLER(memory_doc_post_handler)
HTTP_SERVER_GATED_HANDLER(memory_bulk_handler)

esp_err_t http_server_register_memory_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/memory", .method = HTTP_GET, .handler = gated_memory_list_handler },
        { .uri = "/api/memory", .method = HTTP_POST, .handler = gated_memory_create_handler },
        { .uri = "/api/memory", .method = HTTP_PUT, .handler = gated_memory_update_handler },
        { .uri = "/api/memory", .method = HTTP_DELETE, .handler = gated_memory_delete_handler },
        { .uri = "/api/memory/doc", .method = HTTP_GET, .handler = gated_memory_doc_get_handler },
        { .uri = "/api/memory/doc", .method = HTTP_POST, .handler = gated_memory_doc_post_handler },
        { .uri = "/api/memory/bulk", .method = HTTP_POST, .handler = gated_memory_bulk_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
