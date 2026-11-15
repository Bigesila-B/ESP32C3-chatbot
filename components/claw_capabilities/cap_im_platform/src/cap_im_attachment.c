/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_attachment.h"
#include "claw_image_cache.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

typedef struct {
    claw_image_cache_writer_t writer;
    const char *path;
    size_t max_bytes;
    esp_err_t write_error;
} cap_im_attachment_download_t;

static uint64_t cap_im_attachment_fnv1a64(const char *text)
{
    uint64_t hash = 1469598103934665603ULL;

    if (!text) {
        return hash;
    }

    while (*text) {
        hash ^= (unsigned char)(*text++);
        hash *= 1099511628211ULL;
    }

    return hash;
}

static void cap_im_attachment_sanitize_component(const char *src,
                                                 char *dst,
                                                 size_t dst_size)
{
    size_t written = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src || !src[0]) {
        strlcpy(dst, "unknown", dst_size);
        return;
    }

    while (*src && written + 1 < dst_size) {
        unsigned char ch = (unsigned char) * src++;

        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            dst[written++] = (char)ch;
        } else {
            dst[written++] = '_';
        }
    }

    dst[written] = '\0';
    if (written == 0) {
        strlcpy(dst, "unknown", dst_size);
    }
}

static const char *cap_im_attachment_basename(const char *path)
{
    const char *slash = NULL;

    if (!path || !path[0]) {
        return "";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *cap_im_attachment_find_extension(const char *candidate)
{
    const char *basename = NULL;
    const char *end = NULL;
    const char *dot = NULL;

    if (!candidate || !candidate[0]) {
        return NULL;
    }

    basename = cap_im_attachment_basename(candidate);
    end = basename + strcspn(basename, "?#");
    if (end <= basename) {
        return NULL;
    }

    for (dot = end; dot > basename; dot--) {
        if (dot[-1] == '.') {
            size_t ext_len = (size_t)(end - (dot - 1));

            if (ext_len <= 1 || ext_len > 8) {
                return NULL;
            }
            return dot - 1;
        }
        if (dot[-1] == '/' || dot[-1] == '\\') {
            break;
        }
    }

    return NULL;
}

const char *cap_im_attachment_ext_from_mime(const char *mime)
{
    if (!mime || !mime[0]) {
        return ".bin";
    }
    if (strcmp(mime, "image/jpeg") == 0) {
        return ".jpg";
    }
    if (strcmp(mime, "image/png") == 0) {
        return ".png";
    }
    if (strcmp(mime, "image/gif") == 0) {
        return ".gif";
    }
    if (strcmp(mime, "image/webp") == 0) {
        return ".webp";
    }
    if (strcmp(mime, "audio/mpeg") == 0) {
        return ".mp3";
    }
    if (strcmp(mime, "audio/mp3") == 0) {
        return ".mp3";
    }
    if (strcmp(mime, "audio/wav") == 0 || strcmp(mime, "audio/x-wav") == 0) {
        return ".wav";
    }
    if (strcmp(mime, "audio/ogg") == 0) {
        return ".ogg";
    }
    if (strcmp(mime, "audio/aac") == 0) {
        return ".aac";
    }
    if (strcmp(mime, "audio/amr") == 0) {
        return ".amr";
    }
    if (strcmp(mime, "audio/silk") == 0) {
        return ".silk";
    }
    if (strcmp(mime, "audio/mp4") == 0) {
        return ".m4a";
    }
    if (strcmp(mime, "application/pdf") == 0) {
        return ".pdf";
    }
    if (strcmp(mime, "text/plain") == 0) {
        return ".txt";
    }
    return ".bin";
}

const char *cap_im_attachment_guess_extension(const char *path_or_url,
                                              const char *original_filename,
                                              const char *mime)
{
    const char *candidates[2] = {original_filename, path_or_url};
    size_t i;

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *candidate = candidates[i];
        const char *dot = NULL;

        if (!candidate || !candidate[0]) {
            continue;
        }
        dot = cap_im_attachment_find_extension(candidate);
        if (dot && dot[1]) {
            return dot;
        }
    }

    return cap_im_attachment_ext_from_mime(mime);
}

const char *cap_im_attachment_normalize_url(const char *url,
                                            char *buf,
                                            size_t buf_size)
{
    if (!url || !url[0] || !buf || buf_size == 0) {
        return NULL;
    }

    if (strncmp(url, "//", 2) == 0) {
        snprintf(buf, buf_size, "https:%s", url);
    } else {
        snprintf(buf, buf_size, "%s", url);
    }

    return buf;
}

esp_err_t cap_im_attachment_build_saved_paths(const char *root_dir,
                                              const char *platform_dir,
                                              const char *chat_id,
                                              const char *message_id,
                                              const char *kind,
                                              const char *extension,
                                              char *saved_dir,
                                              size_t saved_dir_size,
                                              char *saved_name,
                                              size_t saved_name_size,
                                              char *saved_path,
                                              size_t saved_path_size)
{
    char safe_chat[64];
    uint64_t message_hash;
    int written;

    if (!root_dir || !root_dir[0] || !platform_dir || !platform_dir[0] || !chat_id ||
            !message_id || !kind || !extension || !saved_dir || !saved_name || !saved_path) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_im_attachment_sanitize_component(chat_id, safe_chat, sizeof(safe_chat));
    written = snprintf(saved_dir, saved_dir_size, "%s/%s/%s", root_dir, platform_dir, safe_chat);
    if (written < 0 || (size_t)written >= saved_dir_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    message_hash = cap_im_attachment_fnv1a64(message_id);
    written = snprintf(saved_name,
                       saved_name_size,
                       "%s_%08" PRIx32 "_%s%s",
                       platform_dir,
                       (uint32_t)message_hash,
                       kind,
                       extension);
    if (written < 0 || (size_t)written >= saved_name_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(saved_path, saved_path_size, "%s/%s", saved_dir, saved_name);
    if (written < 0 || (size_t)written >= saved_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_im_attachment_download_event_handler(esp_http_client_event_t *event)
{
    cap_im_attachment_download_t *dl = event->user_data;
    if (!dl || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    int status = esp_http_client_get_status_code(event->client);
    if (status < 200 || status >= 300 || dl->write_error != ESP_OK) {
        return dl->write_error;
    }
    if (!dl->writer.file) {
        int64_t length = esp_http_client_get_content_length(event->client);
        dl->write_error = length > (int64_t)dl->max_bytes ? ESP_ERR_INVALID_SIZE :
                          claw_image_cache_begin(&dl->writer, dl->path, length > 0 ? (size_t)length : 0, dl->max_bytes);
    }
    if (dl->write_error == ESP_OK) {
        dl->write_error = claw_image_cache_write(&dl->writer, event->data, (size_t)event->data_len);
    }
    return dl->write_error;
}

esp_err_t cap_im_attachment_download_url_to_file(const char *log_tag,
                                                 const char *url,
                                                 const char *dest_path,
                                                 size_t max_bytes,
                                                 size_t *out_bytes)
{
    if (!url || !url[0] || !dest_path || !dest_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_im_attachment_download_t dl = {
        .path = dest_path,
        .max_bytes = max_bytes ? max_bytes : (2 * 1024 * 1024),
    };
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = cap_im_attachment_download_event_handler,
        .user_data = &dl,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (dl.write_error != ESP_OK) {
        err = dl.write_error;
    } else if (err == ESP_OK && (status < 200 || status >= 300 || !dl.writer.bytes ||
                                 !esp_http_client_is_complete_data_received(client))) {
        err = ESP_FAIL;
    }
    esp_http_client_cleanup(client);
    if (dl.writer.file) {
        esp_err_t finish = claw_image_cache_finish(&dl.writer, err == ESP_OK);
        if (err == ESP_OK) {
            err = finish;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(log_tag, "attachment download failed: err=%s http=%d bytes=%u",
                 esp_err_to_name(err), status, (unsigned int)dl.writer.bytes);
        return err;
    }
    if (out_bytes) {
        *out_bytes = dl.writer.bytes;
    }
    return ESP_OK;
}

esp_err_t cap_im_attachment_save_buffer_to_file(const char *log_tag,
                                                const char *dest_path,
                                                const unsigned char *buf,
                                                size_t buf_len)
{
    if (!log_tag || !dest_path || !dest_path[0] || (!buf && buf_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    claw_image_cache_writer_t writer;
    esp_err_t err = claw_image_cache_begin(&writer, dest_path, buf_len, buf_len);
    if (err == ESP_OK) {
        err = buf_len ? claw_image_cache_write(&writer, buf, buf_len) : ESP_OK;
        esp_err_t finish = claw_image_cache_finish(&writer, err == ESP_OK);
        if (err == ESP_OK) {
            err = finish;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(log_tag, "attachment save failed: path=%s err=%s", dest_path, esp_err_to_name(err));
    }
    return err;
}

char *cap_im_attachment_build_payload_json(
    const cap_im_attachment_payload_config_t *config)
{
    cJSON *payload = NULL;
    char *payload_json = NULL;

    if (!config || !config->platform || !config->attachment_kind || !config->saved_path ||
            !config->saved_dir || !config->saved_name) {
        return NULL;
    }

    payload = cJSON_CreateObject();
    if (!payload) {
        return NULL;
    }

    cJSON_AddStringToObject(payload, "platform", config->platform);
    cJSON_AddStringToObject(payload, "attachment_kind", config->attachment_kind);
    cJSON_AddStringToObject(payload, "saved_path", config->saved_path);
    cJSON_AddStringToObject(payload, "saved_dir", config->saved_dir);
    cJSON_AddStringToObject(payload, "saved_name", config->saved_name);
    cJSON_AddStringToObject(payload,
                            "original_filename",
                            config->original_filename ? config->original_filename : "");
    cJSON_AddStringToObject(payload, "mime", config->mime ? config->mime : "");
    cJSON_AddNumberToObject(payload, "size_bytes", (double)config->size_bytes);
    cJSON_AddStringToObject(payload, "caption", config->caption ? config->caption : "");
    cJSON_AddNumberToObject(payload, "saved_at_ms", (double)config->saved_at_ms);
    if (config->source_key && config->source_key[0]) {
        cJSON_AddStringToObject(payload,
                                config->source_key,
                                config->source_value ? config->source_value : "");
    }

    payload_json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    return payload_json;
}
