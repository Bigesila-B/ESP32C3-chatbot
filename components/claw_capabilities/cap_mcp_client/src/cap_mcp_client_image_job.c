/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>

#include "claw_task.h"
#include "esp_timer.h"

#include "cap_mcp_client.h"
#include "cap_mcp_client_priv.h"

#define MCP_RECENT_IMAGE_SLOTS 4
#define MCP_IMAGE_JOB_LIMIT 2
#define MCP_IMAGE_POLL_INTERVAL_MS 5000
#define MCP_IMAGE_POLL_TIMEOUT_US (5LL * 60 * 1000 * 1000)

typedef struct {
    char *channel;
    char *chat_id;
    cJSON *query;
    cap_mcp_image_links_t links;
    uint32_t server_revision;
    unsigned int refs;
    bool running;
} recent_image_t;

static cap_mcp_chat_sender_t s_send;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static recent_image_t *s_recent[MCP_RECENT_IMAGE_SLOTS];
static size_t s_next_slot;
static unsigned int s_running;

static const char *json_string(const cJSON *object, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}

void cap_mcp_client_set_chat_sender(cap_mcp_chat_sender_t sender)
{
    s_send = sender;
}

static const char *target_channel(const claw_cap_call_context_t *ctx)
{
    return ctx->target_channel && ctx->target_channel[0] ? ctx->target_channel : ctx->channel;
}

static const char *target_chat(const claw_cap_call_context_t *ctx)
{
    return ctx->target_channel && ctx->target_channel[0] ? ctx->target_chat_id : ctx->chat_id;
}

static void free_links(cap_mcp_image_links_t *links)
{
    for (size_t i = 0; i < links->count; ++i) {
        free(links->urls[i]);
    }
    links->count = 0;
}

static esp_err_t send_links(const recent_image_t *image, const cap_mcp_image_links_t *links)
{
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < links->count; ++i) {
        esp_err_t status = s_send(image->channel, image->chat_id, links->urls[i], true);
        if (status != ESP_OK) {
            err = status;
        }
    }
    return err;
}

static void release_image(recent_image_t *image)
{
    if (!image) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    bool unused = --image->refs == 0;
    portEXIT_CRITICAL(&s_lock);
    if (unused) {
        cJSON_Delete(image->query);
        free_links(&image->links);
        free(image->chat_id);
        free(image->channel);
        free(image);
    }
}

/* Called with s_lock held. */
static size_t find_chat(const char *channel, const char *chat_id)
{
    for (size_t i = 0; i < MCP_RECENT_IMAGE_SLOTS; ++i) {
        if (s_recent[i] && strcmp(s_recent[i]->channel, channel) == 0 &&
                strcmp(s_recent[i]->chat_id, chat_id) == 0) {
            return i;
        }
    }
    return MCP_RECENT_IMAGE_SLOTS;
}

static bool remember_image(recent_image_t *image)
{
    portENTER_CRITICAL(&s_lock);
    size_t slot = find_chat(image->channel, image->chat_id);
    recent_image_t *previous = slot < MCP_RECENT_IMAGE_SLOTS ? s_recent[slot] : NULL;
    if (previous && image->query && previous->server_revision == image->server_revision &&
            cJSON_Compare(previous->query, image->query, true)) {
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    if (slot == MCP_RECENT_IMAGE_SLOTS) {
        slot = s_next_slot;
        s_next_slot = (s_next_slot + 1) % MCP_RECENT_IMAGE_SLOTS;
        previous = s_recent[slot];
    }
    image->refs++;
    s_recent[slot] = image;
    portEXIT_CRITICAL(&s_lock);
    release_image(previous);
    return true;
}

static esp_err_t fetch_job_image(const recent_image_t *image, cap_mcp_image_links_t *links)
{
    cap_mcp_call_options_t options = { .server_revision = image->server_revision };
    cJSON *result = NULL;
    esp_err_t err = cap_mcp_call_remote_tool(image->query, &options, &result);
    if (err == ESP_OK) {
        cap_mcp_image_result_t remote = {0};
        cap_mcp_image_fields_t fields = {
            .status = json_string(image->query, "status_field"),
            .images = json_string(image->query, "image_field"),
        };
        cap_mcp_read_image(result, strcmp(image->channel, "web") == 0, &fields, &remote);
        if (remote.links.count && remote.status != MCP_IMAGE_FAILED && remote.status != MCP_IMAGE_PENDING) {
            *links = remote.links;
        } else {
            free_links(&remote.links);
            err = remote.status == MCP_IMAGE_FAILED ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_NOT_FINISHED;
        }
    }
    cJSON_Delete(result);
    return err;
}

static void poll_image_job(void *arg)
{
    recent_image_t *image = arg;
    int64_t until = esp_timer_get_time() + MCP_IMAGE_POLL_TIMEOUT_US;
    cap_mcp_image_links_t links = {0};
    esp_err_t err;
    do {
        vTaskDelay(pdMS_TO_TICKS(MCP_IMAGE_POLL_INTERVAL_MS));
        err = fetch_job_image(image, &links);
    } while ((err == ESP_ERR_NOT_FINISHED || err == ESP_ERR_TIMEOUT || err == ESP_FAIL) &&
             esp_timer_get_time() < until);

    if (err == ESP_OK) {
        err = send_links(image, &links);
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        const char *message = links.count ? "图片已生成，发送失败。可用 /mcp last-image 重发。" :
                              err == ESP_ERR_INVALID_RESPONSE ? "MCP 生图任务失败，请检查服务状态。" :
                              "暂未收到生图结果。可用 /mcp last-image 查询并发送。";
        s_send(image->channel, image->chat_id, message, false);
    }
    free_links(&links);
    portENTER_CRITICAL(&s_lock);
    image->running = false;
    s_running--;
    portEXIT_CRITICAL(&s_lock);
    release_image(image);
    claw_task_delete(NULL);
}

static bool start_polling(recent_image_t *image)
{
    portENTER_CRITICAL(&s_lock);
    bool room = s_running < MCP_IMAGE_JOB_LIMIT;
    if (room) {
        s_running++;
        image->running = true;
        image->refs++;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!room) {
        return false;
    }

    static const claw_task_config_t task = {
        .name = "mcp_image",
        .stack_size = 8192,
        .priority = 3,
        .core_id = tskNO_AFFINITY,
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };
    if (claw_task_create(&task, poll_image_job, image, NULL) == pdPASS) {
        return true;
    }
    portENTER_CRITICAL(&s_lock);
    image->running = false;
    s_running--;
    portEXIT_CRITICAL(&s_lock);
    release_image(image);
    return false;
}

static cJSON *make_job_query(const cJSON *input, const cap_mcp_image_delivery_t *delivery, cJSON **job_id)
{
    cJSON *query = cJSON_CreateObject();
    cJSON *args = cJSON_AddObjectToObject(query, "arguments");
    const char *url = json_string(input, "server_url");
    const char *endpoint = json_string(input, "endpoint");
    if (!cJSON_AddStringToObject(query, "tool_name", delivery->status_tool) ||
            !cJSON_AddStringToObject(query, "status_field", delivery->fields.status) ||
            (delivery->fields.images && !cJSON_AddStringToObject(query, "image_field", delivery->fields.images)) ||
            (url && !cJSON_AddStringToObject(query, "server_url", url)) ||
            (endpoint && !cJSON_AddStringToObject(query, "endpoint", endpoint)) ||
            !cJSON_AddItemToObject(args, delivery->id_argument, *job_id)) {
        cJSON_Delete(query);
        return NULL;
    }
    *job_id = NULL;
    return query;
}

bool cap_mcp_deliver_image(const cJSON *input, const cJSON *result,
                           const cap_mcp_call_options_t *options,
                           const claw_cap_call_context_t *ctx, char *output, size_t size)
{
    const char *channel = ctx ? target_channel(ctx) : NULL;
    const char *chat_id = ctx ? target_chat(ctx) : NULL;
    if (!s_send || !channel || !channel[0] || !chat_id || !chat_id[0]) {
        return false;
    }

    cap_mcp_image_result_t generated = {0};
    cap_mcp_image_fields_t fields = options->delivery->fields;
    if (!options->can_poll_image) {
        fields.id = NULL;
    }
    cap_mcp_read_image(result, strcmp(channel, "web") == 0, &fields, &generated);
    bool has_job = generated.job_id && generated.status != MCP_IMAGE_FAILED;
    if (generated.status == MCP_IMAGE_FAILED || generated.status == MCP_IMAGE_PENDING) {
        free_links(&generated.links);
    }
    if (!has_job && !generated.links.count) {
        cJSON_Delete(generated.job_id);
        return false;
    }
    recent_image_t *image = calloc(1, sizeof(*image));
    if (!image) {
        free_links(&generated.links);
        cJSON_Delete(generated.job_id);
        return false;
    }
    image->refs = 1;
    image->channel = strdup(channel);
    image->chat_id = strdup(chat_id);
    image->links = generated.links;
    image->query = has_job ? make_job_query(input, options->delivery, &generated.job_id) : NULL;
    cJSON_Delete(generated.job_id);
    image->server_revision = options->server_revision;
    if (!image->channel || !image->chat_id || (!image->links.count && !image->query)) {
        release_image(image);
        return false;
    }

    if (!remember_image(image)) {
        snprintf(output, size, "MCP image job is already recorded in this chat. Use mcp_last_image to resend.");
    } else if (image->links.count) {
        esp_err_t err = send_links(image, &image->links);
        snprintf(output, size, "%s", err == ESP_OK ?
                 "Image delivered to this chat. Do not send it again." :
                 "Image generated but delivery failed. Use mcp_last_image to resend.");
    } else if (start_polling(image)) {
        snprintf(output, size, "Image job accepted. ESP-Claw will send the image when ready. Do not poll or resend.");
    } else {
        snprintf(output, size, "Image job accepted. The user can check and send it with /mcp last-image or mcp_last_image.");
    }
    release_image(image);
    return true;
}

esp_err_t cap_mcp_client_resend_image(const char *channel, const char *chat_id)
{
    if (!channel || !chat_id || !s_send) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    size_t slot = find_chat(channel, chat_id);
    recent_image_t *image = slot < MCP_RECENT_IMAGE_SLOTS ? s_recent[slot] : NULL;
    bool running = image && image->running;
    if (image) {
        image->refs++;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!image) {
        return ESP_ERR_NOT_FOUND;
    }

    cap_mcp_image_links_t fresh = {0};
    esp_err_t err = running ? ESP_ERR_NOT_FINISHED :
                    image->query ? fetch_job_image(image, &fresh) : ESP_OK;
    if (err == ESP_OK) {
        err = send_links(image, fresh.count ? &fresh : &image->links);
    }
    free_links(&fresh);
    release_image(image);
    return err;
}
