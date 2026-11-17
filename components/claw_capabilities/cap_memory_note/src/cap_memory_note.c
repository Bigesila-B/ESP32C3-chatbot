/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_memory_note.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_memory.h"
#include "esp_log.h"

/*
 * 让模型自己把「值得长期记住的事实」写进长期记忆。
 *
 * 为什么用工具而不是后台抽取：C3 没有 PSRAM，再起一个 LLM runtime 做异步抽取
 * 会直接把堆打爆。这个方案完全复用主 agent 已经打通的 tool calling 链路，
 * 只在模型判断需要时多一次工具执行，峰值内存就是 claw_cap 的那块输出缓冲。
 *
 * 去重、时间戳、MEMORY.md 渲染、容量裁剪都由 claw_memory_store_with_result
 * 内部处理，这里不做第二套判断。
 */
static const char *TAG = "cap_memory_note";

/* 单条记忆上限：与 claw_memory_item_t.content[256] 保持一致并留一点余量 */
#define CAP_MEMORY_NOTE_MAX_BYTES 220

static esp_err_t cap_memory_note_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    cJSON *root = NULL;
    const char *content = NULL;
    claw_memory_item_t item = {0};
    bool changed = false;
    esp_err_t err;

    (void)ctx;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "content"));
    if (!content || !content[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: content is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(content) > CAP_MEMORY_NOTE_MAX_BYTES) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: memory text is too long (max %d bytes)",
                 CAP_MEMORY_NOTE_MAX_BYTES);
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(item.content, content, sizeof(item.content));
    /* source 用来区分网页手工写入与模型自动记录，方便用户在后台辨认 */
    strlcpy(item.source, "auto", sizeof(item.source));

    err = claw_memory_store_with_result(&item, &changed);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to store memory: %s", esp_err_to_name(err));
        snprintf(output, output_size, "Error: failed to save memory (%s)",
                 esp_err_to_name(err));
        return err;
    }

    /* 告诉模型这次是新增还是已存在，避免它下一轮重复调用 */
    snprintf(output, output_size, "%s: %s",
             changed ? "Saved to long-term memory" : "Already in long-term memory",
             item.content);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_memory_note_descriptors[] = {
    {
        .id = "memory_remember",
        .name = "memory_remember",
        .family = "memory",
        .description =
            "Save ONE durable fact about the user to long-term memory, such as their name, "
            "role, current project, hardware they own, or a stable preference. "
            "Call this when the user states such a fact or explicitly asks you to remember something. "
            "Do NOT save greetings, questions, transient tasks, assistant opinions, or anything "
            "already known. The content MUST be written in the SAME language as the user's "
            "message (Chinese when the user writes Chinese) — never translate it to English. "
            "Keep the text short, factual and self-contained.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"}},"
            "\"required\":[\"content\"]}",
        .execute = cap_memory_note_execute,
    },
};

static const claw_cap_group_t s_memory_note_group = {
    .group_id = "cap_memory_note",
    .descriptors = s_memory_note_descriptors,
    .descriptor_count = sizeof(s_memory_note_descriptors) / sizeof(s_memory_note_descriptors[0]),
};

esp_err_t cap_memory_note_register_group(void)
{
    if (claw_cap_group_exists(s_memory_note_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_memory_note_group);
}
