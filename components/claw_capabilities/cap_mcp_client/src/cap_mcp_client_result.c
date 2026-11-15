/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "http_parser.h"
#include "mbedtls/base64.h"

#include "cap_mcp_client.h"
#include "cap_mcp_client_priv.h"

#define MCP_IMAGE_CACHE_SLOTS  4
#define MCP_INLINE_IMAGE_MAX   (64 * 1024)
#define MCP_IMAGE_URL_MAX      1500
#define MCP_RESULT_DEPTH_MAX   8
#define MCP_STRUCTURED_TEXT_MAX (8 * 1024)

typedef struct {
    const char *mime;
    const unsigned char *signature;
    size_t signature_size;
    size_t signature_offset;
} image_type_t;

struct cap_mcp_image {
    uint32_t id;
    unsigned int refs;
    const char *mime;
    size_t size;
    unsigned char data[];
};

typedef struct {
    char *output;
    const claw_cap_call_context_t *context;
    cap_mcp_image_result_t *generated;
    const cap_mcp_image_fields_t *fields;
    size_t size;
    size_t used;
    unsigned int images;
    bool web_chat;
    bool inline_unavailable;
} result_writer_t;

static portMUX_TYPE s_image_lock = portMUX_INITIALIZER_UNLOCKED;
static cap_mcp_image_t *s_images[MCP_IMAGE_CACHE_SLOTS];
static size_t s_next_image_slot;

static const char *json_string(const cJSON *object, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}

cap_mcp_image_t *cap_mcp_client_image_acquire(uint32_t id)
{
    cap_mcp_image_t *image = NULL;
    portENTER_CRITICAL(&s_image_lock);
    for (size_t i = 0; i < MCP_IMAGE_CACHE_SLOTS; i++) {
        if (s_images[i] && s_images[i]->id == id) {
            image = s_images[i];
            image->refs++;
            break;
        }
    }
    portEXIT_CRITICAL(&s_image_lock);
    return image;
}

const void *cap_mcp_client_image_data(const cap_mcp_image_t *image, size_t *size, const char **mime)
{
    *size = image->size;
    *mime = image->mime;
    return image->data;
}

void cap_mcp_client_image_release(cap_mcp_image_t *image)
{
    if (!image) {
        return;
    }
    portENTER_CRITICAL(&s_image_lock);
    bool unused = --image->refs == 0;
    portEXIT_CRITICAL(&s_image_lock);
    if (unused) {
        free(image);
    }
}

static const image_type_t *get_image_type(const char *mime)
{
    static const image_type_t types[] = {
        {"image/png",  (const unsigned char *)"\x89PNG\r\n\x1a\n", 8, 0},
        {"image/jpeg", (const unsigned char *)"\xff\xd8\xff", 3, 0},
        {"image/gif",  (const unsigned char *)"GIF8", 4, 0},
        {"image/webp", (const unsigned char *)"WEBP", 4, 8},
    };
    for (size_t i = 0; mime && i < sizeof(types) / sizeof(types[0]); i++) {
        if (strcasecmp(mime, types[i].mime) == 0) {
            return &types[i];
        }
    }
    return NULL;
}

static bool cache_inline_image(const char *encoded, const char *mime, uint32_t *id)
{
    const image_type_t *type = get_image_type(mime);
    size_t size = 0;
    if (!encoded || !type ||
            mbedtls_base64_decode(NULL, 0, &size, (const unsigned char *)encoded, strlen(encoded)) !=
                MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL ||
            size > MCP_INLINE_IMAGE_MAX || size < type->signature_offset + type->signature_size) {
        return false;
    }

    /* Keep the cache in external RAM. On boards without it, use remote links. */
    cap_mcp_image_t *image = heap_caps_malloc(sizeof(*image) + size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!image) {
        return false;
    }
    if (mbedtls_base64_decode(image->data, size, &image->size,
                              (const unsigned char *)encoded, strlen(encoded)) != 0 ||
            memcmp(image->data + type->signature_offset, type->signature, type->signature_size) != 0) {
        free(image);
        return false;
    }
    image->mime = type->mime;
    image->refs = 1;
    image->id = esp_random();
    *id = image->id;

    portENTER_CRITICAL(&s_image_lock);
    cap_mcp_image_t *old = s_images[s_next_image_slot];
    s_images[s_next_image_slot] = image;
    s_next_image_slot = (s_next_image_slot + 1) % MCP_IMAGE_CACHE_SLOTS;
    portEXIT_CRITICAL(&s_image_lock);
    cap_mcp_client_image_release(old);
    return true;
}

static void append_text(result_writer_t *writer, const char *text)
{
    if (!writer->output || !text || !text[0] || writer->used >= writer->size - 1) {
        return;
    }
    if (writer->used && writer->output[writer->used - 1] != '\n') {
        writer->output[writer->used++] = '\n';
    }
    size_t room = writer->size - 1 - writer->used;
    size_t len = strnlen(text, room + 1);
    if (len > room) {
        len = room;
        while (len && ((unsigned char)text[len] & 0xc0) == 0x80) {
            len--;
        }
    }
    memcpy(writer->output + writer->used, text, len);
    writer->used += len;
    writer->output[writer->used] = '\0';
}

static bool remote_image_url(const char *url)
{
    if (!url || strlen(url) > MCP_IMAGE_URL_MAX || strpbrk(url, "<>\\ \t\r\n") ||
            (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)) {
        return false;
    }
    struct http_parser_url parsed;
    http_parser_url_init(&parsed);
    return http_parser_parse_url(url, strlen(url), 0, &parsed) == 0 &&
           (parsed.field_set & (1 << UF_HOST)) && !(parsed.field_set & (1 << UF_USERINFO));
}

static bool has_image_extension(const char *url)
{
    if (!url) {
        return false;
    }
    static const char *const suffixes[] = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".avif", ".bmp"};
    size_t path_len = strcspn(url, "?#");
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        size_t len = strlen(suffixes[i]);
        if (path_len > len && strncasecmp(url + path_len - len, suffixes[i], len) == 0) {
            return true;
        }
    }
    return false;
}

static bool append_image(result_writer_t *writer, const char *url)
{
    if (writer->images >= MCP_RESULT_IMAGE_MAX) {
        return false;
    }
    if (writer->generated) {
        cap_mcp_image_links_t *links = &writer->generated->links;
        for (size_t i = 0; i < links->count; ++i) {
            if (strcmp(links->urls[i], url) == 0) {
                return true;
            }
        }
        char *copy = strdup(url);
        if (!copy) {
            return false;
        }
        links->urls[links->count++] = copy;
        writer->images++;
        return true;
    }

    const char *previous = writer->output;
    size_t url_len = strlen(url);
    while ((previous = strstr(previous, "![MCP image](<"))) {
        previous += sizeof("![MCP image](<") - 1;
        if (strncmp(previous, url, url_len) == 0 && previous[url_len] == '>') {
            return true;
        }
    }
    const char *prefix = writer->used ? "\n![MCP image](<" : "![MCP image](<";
    size_t len = strlen(prefix) + strlen(url) + sizeof(">)");
    if (len > writer->size - writer->used) {
        return false;
    }
    writer->used += (size_t)snprintf(writer->output + writer->used, writer->size - writer->used,
                                    "%s%s>)", prefix, url);
    writer->images++;
    if (writer->context) {
        (void)claw_core_add_reply_image(writer->context->core, writer->context->request_id, url);
    }
    return true;
}

static bool extract_images(const cJSON *value, bool image_context, unsigned int depth, result_writer_t *writer)
{
    if (writer->generated && (writer->generated->status == MCP_IMAGE_PENDING ||
                               writer->generated->status == MCP_IMAGE_FAILED)) {
        return false;
    }
    if (depth > MCP_RESULT_DEPTH_MAX || writer->images >= MCP_RESULT_IMAGE_MAX) {
        return false;
    }
    if (cJSON_IsString(value)) {
        const char *url = value->valuestring;
        return (image_context || has_image_extension(url)) && remote_image_url(url) && append_image(writer, url);
    }
    bool found = false;
    if (cJSON_IsArray(value)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, value) {
            found |= extract_images(item, image_context, depth + 1, writer);
            if (writer->images >= MCP_RESULT_IMAGE_MAX) {
                break;
            }
        }
        return found;
    }
    if (!cJSON_IsObject(value)) {
        return false;
    }

    const char *mime = json_string(value, "mimeType");
    if (!mime) {
        mime = json_string(value, "mime_type");
    }
    const char *kind = json_string(value, "kind");
    const char *type = json_string(value, "type");
    bool image = image_context || (mime && strncasecmp(mime, "image/", 6) == 0) ||
                 (kind && strcmp(kind, "image") == 0) || (type && strcmp(type, "image") == 0);
    const char *data = json_string(value, "data");
    const char *blob = json_string(value, "blob");
    const char *encoded = type && strcmp(type, "image") == 0 && data ? data : blob;
    if (encoded && image) {
        uint32_t id;
        if (writer->web_chat && cache_inline_image(encoded, mime, &id)) {
            char url[40];
            snprintf(url, sizeof(url), "/api/mcp/image?id=%08" PRIx32, id);
            return append_image(writer, url);
        }
        writer->inline_unavailable = true;
    }
    const char *url = json_string(value, "url");
    const char *download = json_string(value, "download_url");
    const char *uri = json_string(value, "uri");
    if ((image || has_image_extension(url) || has_image_extension(json_string(value, "path"))) &&
            remote_image_url(download)) {
        found |= append_image(writer, download);
    } else if ((image || has_image_extension(url)) && remote_image_url(url)) {
        found |= append_image(writer, url);
    } else if ((image || has_image_extension(uri)) && remote_image_url(uri)) {
        found |= append_image(writer, uri);
    } else if (has_image_extension(download) && remote_image_url(download)) {
        found |= append_image(writer, download);
    }

    static const char *const containers[] = {"image_url", "image", "images", "result", "asset", "files",
                                              "items", "output", "resource", "data", "job"};
    for (size_t i = 0; i < sizeof(containers) / sizeof(containers[0]); i++) {
        const cJSON *child = cJSON_GetObjectItemCaseSensitive(value, containers[i]);
        if (child && !(data && cJSON_IsString(child) && child->valuestring == data)) {
            found |= extract_images(child, image || i < 3, depth + 1, writer);
        }
    }
    return found;
}

static void append_job_status(result_writer_t *writer, const cJSON *value)
{
    const char *job = json_string(value, "job_id");
    const char *status = json_string(value, "status");
    char summary[112];
    if (job && status) {
        snprintf(summary, sizeof(summary), "Job %.72s: %.24s", job, status);
        append_text(writer, summary);
    }
}

static const cJSON *result_field(const cJSON *value, const char *path)
{
    if (!path) {
        return NULL;
    }
    while (cJSON_IsObject(value)) {
        const char *dot = strchr(path, '.');
        if (!dot) {
            return cJSON_GetObjectItemCaseSensitive(value, path);
        }
        size_t length = (size_t)(dot - path);
        const cJSON *entry;
        cJSON_ArrayForEach(entry, value) {
            if (entry->string && strncmp(entry->string, path, length) == 0 && entry->string[length] == '\0') {
                break;
            }
        }
        value = entry;
        path = dot + 1;
    }
    return NULL;
}

static void read_job_status(result_writer_t *writer, const cJSON *value)
{
    cap_mcp_image_result_t *image = writer->generated;
    if (!image || !value) {
        return;
    }
    if (!image->job_id && writer->fields->id) {
        const cJSON *id = result_field(value, writer->fields->id);
        const char *text = cJSON_GetStringValue(id);
        if ((text && text[0] && strlen(text) <= MCP_IMAGE_FIELD_MAX) ||
                (cJSON_IsNumber(id) && isfinite(id->valuedouble))) {
            image->job_id = cJSON_Duplicate(id, false);
        }
    }
    const char *status = cJSON_GetStringValue(result_field(value, writer->fields->status));
    if (!status || !status[0] || image->status != MCP_IMAGE_NO_STATUS) {
        return;
    }
    if (strcasecmp(status, "succeeded") == 0 || strcasecmp(status, "completed") == 0 ||
            strcasecmp(status, "success") == 0 || strcasecmp(status, "done") == 0 || strcasecmp(status, "complete") == 0 ||
            strcasecmp(status, "ready") == 0 || strcasecmp(status, "finished") == 0) {
        image->status = MCP_IMAGE_READY;
    } else if (strcasecmp(status, "failed") == 0 || strcasecmp(status, "error") == 0 ||
               strcasecmp(status, "cancelled") == 0 || strcasecmp(status, "canceled") == 0 ||
               strcasecmp(status, "rejected") == 0 || strcasecmp(status, "expired") == 0) {
        image->status = MCP_IMAGE_FAILED;
    } else {
        image->status = MCP_IMAGE_PENDING;
    }
}

static bool extract_result_images(const cJSON *value, result_writer_t *writer)
{
    const char *path = writer->fields ? writer->fields->images : NULL;
    return extract_images(path ? result_field(value, path) : value, path != NULL, 0, writer);
}

static bool extract_text_image(const char *text, result_writer_t *writer)
{
    if (!writer->generated || writer->fields->images ||
            writer->generated->status == MCP_IMAGE_PENDING || writer->generated->status == MCP_IMAGE_FAILED ||
            (strncmp(text, "http://", 7) != 0 && strncmp(text, "https://", 8) != 0)) {
        return false;
    }
    size_t length = strcspn(text, " \t\r\n");
    if (length > MCP_IMAGE_URL_MAX || text[length + strspn(text + length, " \t\r\n")]) {
        return false;
    }
    char *url = malloc(length + 1);
    if (!url) {
        return false;
    }
    memcpy(url, text, length);
    url[length] = '\0';
    bool found = remote_image_url(url) && append_image(writer, url);
    free(url);
    return found;
}

static void format_result(const cJSON *result, result_writer_t *writer)
{
    const char *error = json_string(result, "error_message");
    if (error && error[0]) {
        if (writer->output) {
            snprintf(writer->output, writer->size, "Error: %s", error);
        } else {
            writer->generated->status = MCP_IMAGE_FAILED;
        }
        return;
    }

    bool is_error = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError"));
    if (is_error) {
        if (writer->generated) {
            writer->generated->status = MCP_IMAGE_FAILED;
            return;
        }
        append_text(writer, "Error: MCP tool returned an application error");
    }

    const cJSON *structured = cJSON_GetObjectItemCaseSensitive(result, "structuredContent");
    read_job_status(writer, structured);
    read_job_status(writer, cJSON_GetObjectItemCaseSensitive(structured, "job"));
    bool structured_image = !is_error && extract_result_images(structured, writer);
    if (structured_image || writer->inline_unavailable) {
        append_job_status(writer, structured);
    }

    const cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    const cJSON *item;
    cJSON_ArrayForEach(item, content) {
        const char *type = json_string(item, "type");
        if (!type) {
            continue;
        }
        if (strcmp(type, "text") == 0) {
            const char *message = json_string(item, "text");
            if (!message) {
                continue;
            }
            const char *start = message + strspn(message, " \t\r\n");
            cJSON *parsed = (!is_error && (*start == '{' || *start == '[')) ?
                            cap_mcp_parse_json(start, strlen(start)) : NULL;
            read_job_status(writer, parsed);
            read_job_status(writer, cJSON_GetObjectItemCaseSensitive(parsed, "job"));
            bool mirrored = parsed && (structured_image || writer->inline_unavailable) &&
                            cJSON_Compare(parsed, structured, true);
            bool inline_unavailable = writer->inline_unavailable;
            writer->inline_unavailable = false;
            bool image = !mirrored && (parsed ? extract_result_images(parsed, writer) :
                                                !is_error && extract_text_image(start, writer));
            if ((image || writer->inline_unavailable) && !structured_image) {
                append_job_status(writer, parsed);
            }
            if (!image && !mirrored && !writer->inline_unavailable) {
                append_text(writer, message);
            }
            writer->inline_unavailable |= inline_unavailable;
            cJSON_Delete(parsed);
        } else if (!is_error && (strcmp(type, "image") == 0 || strcmp(type, "resource") == 0 ||
                                  strcmp(type, "resource_link") == 0)) {
            extract_images(item, false, 0, writer);
        }
    }

    if (!writer->output) {
        return;
    }
    if (writer->inline_unavailable) {
        append_text(writer, writer->web_chat ?
                    "An inline image was unavailable. Ask for a public image URL or a PNG/JPEG/GIF/WebP up to 64 KiB." :
                    "This chat needs a public image URL for MCP images. Ask the server for a downloadable link.");
    }
    if (!writer->used && structured) {
        char *json = cJSON_PrintUnformatted(structured);
        if (json) {
            writer->size = writer->size < MCP_STRUCTURED_TEXT_MAX ? writer->size : MCP_STRUCTURED_TEXT_MAX;
            append_text(writer, json);
            cJSON_free(json);
        }
    }
    if (!writer->used) {
        append_text(writer, "(empty)");
    }
}

void cap_mcp_read_image(const cJSON *result, bool web, const cap_mcp_image_fields_t *fields,
                         cap_mcp_image_result_t *image)
{
    result_writer_t writer = { .web_chat = web, .generated = image, .fields = fields };
    format_result(result, &writer);
}

void cap_mcp_format_result(const cJSON *result, const claw_cap_call_context_t *ctx,
                           char *output, size_t size)
{
    if (!output || !size) {
        return;
    }
    output[0] = '\0';
    const char *channel = ctx ? (ctx->target_channel && ctx->target_channel[0] ?
                                 ctx->target_channel : ctx->channel) : NULL;
    bool web = channel && strcmp(channel, "web") == 0;
    result_writer_t writer = { .output = output, .size = size, .context = web ? ctx : NULL, .web_chat = web };
    format_result(result, &writer);
}
