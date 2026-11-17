/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "http_server_priv.h"

/*
 * 网页后台的聊天记录接口。
 *
 * 会话文件由 claw_memory 写入，命名规则是
 *     <DATA>/sessions/s_<session_id>_<8位哈希>.log
 * 记录是每行一个 JSON 对象，至少包含 role / content 两个字段。
 *
 * 这里不重复实现那份哈希算法，改为扫描目录、从文件名反解 session_id 再比对，
 * 既避免两处逻辑漂移，也不会因为外部传入任意文件名而留下路径穿越的口子。
 */

static const char *TAG = "http_session";

#define SESSION_DIR_REL           "/sessions"
#define SESSION_FILE_PREFIX       "s_"
#define SESSION_FILE_SUFFIX       ".log"
#define SESSION_ID_SAFE_MAX       32
#define SESSION_LIST_MAX          16
#define SESSION_TITLE_MAX         48
#define SESSION_LINE_BUF          4096
/* 回显条数与单条长度都要克制：C3 没有 PSRAM，响应整串是一次性 malloc 出来的 */
#define SESSION_REPLAY_MAX        30
#define SESSION_REPLAY_TEXT_MAX   320
#define SESSION_TITLE_SCAN_LINES  12

typedef struct {
    char id[SESSION_ID_SAFE_MAX + 1];
    char file[64];
    char title[SESSION_TITLE_MAX + 1];
    long long updated;
} session_entry_t;

/* 会话 id 只允许字母数字、下划线和短横线，长度 1..32，供前端生成 */
static bool session_id_valid(const char *id)
{
    size_t len;

    if (!id || !id[0]) {
        return false;
    }
    len = strlen(id);
    if (len > SESSION_ID_SAFE_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)id[i];

        if (!isalnum(ch) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

/* 从 s_<id>_<hash>.log 反解出 <id>；解析不出来就返回 false */
static bool session_id_from_file(const char *file, char *out, size_t out_size)
{
    const size_t prefix_len = strlen(SESSION_FILE_PREFIX);
    const size_t suffix_len = strlen(SESSION_FILE_SUFFIX);
    size_t name_len;
    size_t sep;

    if (!file || !out || out_size == 0) {
        return false;
    }
    name_len = strlen(file);
    if (name_len <= prefix_len + suffix_len ||
            strncmp(file, SESSION_FILE_PREFIX, prefix_len) != 0 ||
            strcmp(file + name_len - suffix_len, SESSION_FILE_SUFFIX) != 0) {
        return false;
    }

    /* 去掉后缀后的有效长度 */
    name_len -= suffix_len;

    /* 从右往左找最后一处下划线，它是 id 与哈希的分隔符 */
    sep = name_len;
    while (sep > prefix_len && file[sep] != '_') {
        sep--;
    }
    if (sep <= prefix_len) {
        return false;
    }

    /* 分隔符右侧必须正好是 8 位十六进制哈希 */
    if (name_len - (sep + 1) != 8) {
        return false;
    }
    for (size_t i = sep + 1; i < name_len; i++) {
        if (!isxdigit((unsigned char)file[i])) {
            return false;
        }
    }

    if (sep - prefix_len == 0 || sep - prefix_len >= out_size) {
        return false;
    }
    memcpy(out, file + prefix_len, sep - prefix_len);
    out[sep - prefix_len] = '\0';
    return true;
}

static esp_err_t session_dir_path(char *out, size_t out_size)
{
    return http_server_resolve_storage_path(SESSION_DIR_REL, out, out_size);
}

static void session_entry_path(const char *file, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s%s/%s", http_server_ctx()->storage_base_path, SESSION_DIR_REL, file);
}

/*
 * 从会话文件里取第一条用户消息当标题。
 * 只读文件开头若干行，避免为了一个标题把上百 KB 的历史全读一遍。
 */
static void session_scan_title(const char *path, char *out, size_t out_size)
{
    FILE *fp = NULL;
    char *line = NULL;

    out[0] = '\0';
    line = malloc(SESSION_LINE_BUF);
    if (!line) {
        return;
    }
    fp = fopen(path, "r");
    if (!fp) {
        free(line);
        return;
    }

    for (int i = 0; i < SESSION_TITLE_SCAN_LINES && fgets(line, SESSION_LINE_BUF, fp); i++) {
        cJSON *root = cJSON_Parse(line);
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "role"));
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "content"));

        if (role && content && strcmp(role, "user") == 0 && content[0]) {
            size_t n = 0;

            /* 换行和连续空白在列表里没有意义，统一压成单个空格 */
            for (const char *p = content; *p && n < SESSION_TITLE_MAX && n + 1 < out_size; p++) {
                if (*p == '\n' || *p == '\r' || *p == '\t') {
                    if (n > 0 && out[n - 1] != ' ') {
                        out[n++] = ' ';
                    }
                } else {
                    out[n++] = *p;
                }
            }
            out[n] = '\0';
            cJSON_Delete(root);
            break;
        }
        cJSON_Delete(root);
    }

    fclose(fp);
    free(line);
}

/* 按修改时间从新到旧插入排序，列表最多保留 SESSION_LIST_MAX 条 */
static void session_list_insert(session_entry_t *list, size_t *count, const session_entry_t *item)
{
    size_t pos = *count;

    if (*count < SESSION_LIST_MAX) {
        (*count)++;
    } else if (item->updated <= list[*count - 1].updated) {
        return;
    } else {
        pos = *count - 1;
    }

    while (pos > 0 && list[pos - 1].updated < item->updated) {
        list[pos] = list[pos - 1];
        pos--;
    }
    list[pos] = *item;
}

static void session_collect_entries(session_entry_t *list, size_t *count)
{
    char dir_path[HTTP_SERVER_PATH_MAX];
    DIR *dir = NULL;
    struct dirent *entry = NULL;

    *count = 0;
    if (session_dir_path(dir_path, sizeof(dir_path)) != ESP_OK) {
        return;
    }
    dir = opendir(dir_path);
    if (!dir) {
        /* 还没有任何会话时目录可能不存在，这不是错误 */
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        session_entry_t item = {0};
        char full_path[HTTP_SERVER_PATH_MAX];
        struct stat st;

        if (!session_id_from_file(entry->d_name, item.id, sizeof(item.id))) {
            continue;
        }
        strlcpy(item.file, entry->d_name, sizeof(item.file));
        session_entry_path(item.file, full_path, sizeof(full_path));
        if (stat(full_path, &st) != 0) {
            continue;
        }
        item.updated = (long long)st.st_mtime;
        session_scan_title(full_path, item.title, sizeof(item.title));
        session_list_insert(list, count, &item);
    }

    closedir(dir);
}

static esp_err_t session_list_handler(httpd_req_t *req)
{
    session_entry_t *list = calloc(SESSION_LIST_MAX, sizeof(*list));
    size_t count = 0;
    cJSON *out = NULL;
    cJSON *array = NULL;

    if (!list) {
        return httpd_resp_send_500(req);
    }
    session_collect_entries(list, &count);

    out = cJSON_CreateObject();
    if (!out) {
        free(list);
        return httpd_resp_send_500(req);
    }
    array = cJSON_AddArrayToObject(out, "sessions");
    if (!array) {
        free(list);
        cJSON_Delete(out);
        return httpd_resp_send_500(req);
    }

    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();

        if (!item) {
            break;
        }
        cJSON_AddStringToObject(item, "id", list[i].id);
        cJSON_AddStringToObject(item, "title", list[i].title[0] ? list[i].title : list[i].id);
        cJSON_AddNumberToObject(item, "updated", (double)list[i].updated);
        cJSON_AddItemToArray(array, item);
    }

    free(list);
    return http_server_send_json_response(req, out);
}

/* 按会话 id 找到对应的磁盘文件，找不到返回 ESP_ERR_NOT_FOUND */
static esp_err_t session_find_file(const char *id, char *out, size_t out_size)
{
    char dir_path[HTTP_SERVER_PATH_MAX];
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    esp_err_t err = ESP_ERR_NOT_FOUND;

    if (!session_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session_dir_path(dir_path, sizeof(dir_path)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    dir = opendir(dir_path);
    if (!dir) {
        return ESP_ERR_NOT_FOUND;
    }

    while ((entry = readdir(dir)) != NULL) {
        char parsed[SESSION_ID_SAFE_MAX + 1];

        if (!session_id_from_file(entry->d_name, parsed, sizeof(parsed))) {
            continue;
        }
        /* 哈希冲突到同名 id 的概率可以忽略，命中即用 */
        if (strcmp(parsed, id) == 0) {
            session_entry_path(entry->d_name, out, out_size);
            err = ESP_OK;
            break;
        }
    }

    closedir(dir);
    return err;
}

/*
 * 读取会话历史。
 * 逐行解析，环形缓冲只保留最后 SESSION_REPLAY_MAX 条 user/assistant 消息，
 * 这样文件再大也只需要一份行缓冲加一份结果。
 */
static esp_err_t session_read_handler(httpd_req_t *req)
{
    char id[SESSION_ID_SAFE_MAX + 1];
    char path[HTTP_SERVER_PATH_MAX];
    char *line = NULL;
    FILE *fp = NULL;
    cJSON **ring = NULL;
    size_t head = 0;
    size_t stored = 0;
    cJSON *out = NULL;
    cJSON *array = NULL;

    if (http_server_query_get(req, "id", id, sizeof(id)) != ESP_OK || !session_id_valid(id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid session id");
    }
    if (session_find_file(id, path, sizeof(path)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Session not found");
    }

    line = malloc(SESSION_LINE_BUF);
    ring = calloc(SESSION_REPLAY_MAX, sizeof(*ring));
    if (!line || !ring) {
        free(line);
        free(ring);
        return httpd_resp_send_500(req);
    }

    fp = fopen(path, "r");
    if (!fp) {
        free(line);
        free(ring);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open session");
    }

    while (fgets(line, SESSION_LINE_BUF, fp)) {
        cJSON *root = cJSON_Parse(line);
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "role"));
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "content"));

        if (!root || !role || !content || !content[0] ||
                (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0)) {
            cJSON_Delete(root);
            continue;
        }

        cJSON *msg = cJSON_CreateObject();
        if (!msg) {
            cJSON_Delete(root);
            break;
        }
        cJSON_AddStringToObject(msg, "role", role);
        /* 超长回复截断，前端只要能看到大致内容即可 */
        if (strlen(content) > SESSION_REPLAY_TEXT_MAX) {
            char truncated[SESSION_REPLAY_TEXT_MAX + 1];

            memcpy(truncated, content, SESSION_REPLAY_TEXT_MAX);
            truncated[SESSION_REPLAY_TEXT_MAX] = '\0';
            cJSON_AddStringToObject(msg, "content", truncated);
        } else {
            cJSON_AddStringToObject(msg, "content", content);
        }

        cJSON_Delete(ring[head]);
        ring[head] = msg;
        head = (head + 1) % SESSION_REPLAY_MAX;
        if (stored < SESSION_REPLAY_MAX) {
            stored++;
        }
        cJSON_Delete(root);
    }
    fclose(fp);
    free(line);

    out = cJSON_CreateObject();
    if (!out) {
        for (size_t i = 0; i < SESSION_REPLAY_MAX; i++) {
            cJSON_Delete(ring[i]);
        }
        free(ring);
        return httpd_resp_send_500(req);
    }
    array = cJSON_AddArrayToObject(out, "messages");
    if (!array) {
        for (size_t i = 0; i < SESSION_REPLAY_MAX; i++) {
            cJSON_Delete(ring[i]);
        }
        free(ring);
        cJSON_Delete(out);
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(out, "id", id);

    /* 环形缓冲按写入顺序回放，起点是 head 回退 stored 条 */
    size_t start = (head + SESSION_REPLAY_MAX - stored) % SESSION_REPLAY_MAX;
    for (size_t i = 0; i < stored; i++) {
        cJSON *msg = ring[(start + i) % SESSION_REPLAY_MAX];

        if (msg) {
            cJSON_AddItemToArray(array, msg);
            ring[(start + i) % SESSION_REPLAY_MAX] = NULL;
        }
    }
    free(ring);
    return http_server_send_json_response(req, out);
}

static esp_err_t session_delete_handler(httpd_req_t *req)
{
    char id[SESSION_ID_SAFE_MAX + 1];
    char path[HTTP_SERVER_PATH_MAX];
    char aux[HTTP_SERVER_PATH_MAX + 16];
    bool removed = false;
    esp_err_t err;

    if (http_server_query_get(req, "id", id, sizeof(id)) != ESP_OK || !session_id_valid(id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid session id");
    }
    err = session_find_file(id, path, sizeof(path));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Session not found");
    }

    if (unlink(path) == 0) {
        removed = true;
    }
    /* 索引、摘要与阻塞标记都是会话的附属文件，一并清掉，避免残留 */
    snprintf(aux, sizeof(aux), "%s.idx", path);
    unlink(aux);
    snprintf(aux, sizeof(aux), "%s.summary.txt", path);
    unlink(aux);
    snprintf(aux, sizeof(aux), "%s.blocked", path);
    unlink(aux);

    if (!removed) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete session");
    }

    ESP_LOGI(TAG, "session deleted: %s", id);
    cJSON *out = cJSON_CreateObject();

    if (!out) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "id", id);
    return http_server_send_json_response(req, out);
}

esp_err_t http_server_register_session_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/sessions", .method = HTTP_GET, .handler = session_list_handler },
        { .uri = "/api/session", .method = HTTP_GET, .handler = session_read_handler },
        { .uri = "/api/session", .method = HTTP_DELETE, .handler = session_delete_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);

        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
