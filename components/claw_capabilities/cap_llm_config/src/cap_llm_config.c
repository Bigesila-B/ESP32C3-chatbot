/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_llm_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "cap_llm_config";

static cap_llm_config_provider_t s_provider;

typedef struct {
    const char *id;
    const char *backend_type;
    const char *base_url;
    const char *auth_type;
    const char *model;
    const char *max_tokens_field;
    const char *default_image_max_bytes;
    const char *supports_tools;
    const char *supports_vision;
    const char *image_remote_url_only;
} cap_llm_preset_t;

typedef struct {
    char provider[CAP_LLM_CONFIG_SHORT_STR_LEN];
    char token[CAP_LLM_CONFIG_STR_LEN];
    char model[CAP_LLM_CONFIG_MODEL_LEN];
    char backend[CAP_LLM_CONFIG_SHORT_STR_LEN];
    char url[CAP_LLM_CONFIG_STR_LEN];
    char base_url[CAP_LLM_CONFIG_STR_LEN];
    char custom_model[CAP_LLM_CONFIG_MODEL_LEN];
    char custom_token[CAP_LLM_CONFIG_STR_LEN];
} cap_llm_config_parse_buf_t;

static const cap_llm_preset_t s_presets[] = {
    {
        .id = "deepseek",
        .backend_type = "openai_compatible",
        .base_url = "https://api.deepseek.com",
        .auth_type = "bearer",
        .model = "deepseek-chat",
        .max_tokens_field = "max_completion_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
    {
        .id = "openai",
        .backend_type = "openai_compatible",
        .base_url = "https://api.openai.com/v1",
        .auth_type = "bearer",
        .model = "gpt-4o-mini",
        .max_tokens_field = "max_completion_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
    {
        .id = "qwen",
        .backend_type = "openai_compatible",
        .base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
        .auth_type = "bearer",
        .model = "qwen-plus",
        .max_tokens_field = "max_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
    {
        .id = "anthropic",
        .backend_type = "anthropic_compatible",
        .base_url = "https://api.anthropic.com/v1",
        .auth_type = "none",
        .model = "claude-3-5-haiku-latest",
        .max_tokens_field = "max_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
};

#define LLM_CUSTOM_SETUP "/llm setup custom backend=<backend> url=<base_url> model=<model> token=<api_token>"
#define LLM_COMMAND_ROWS(row) \
    row("/llm", "Show model configuration commands", "查看模型配置指令") \
    row("/llm help", "Show model configuration commands", "查看模型配置指令") \
    row("/llm ?", "Alias for /llm help", "查看模型配置指令，与 /llm help 相同") \
    row("/llm status", "Show current model settings", "查看当前模型配置") \
    row("/llm show", "Alias for /llm status", "查看当前模型配置，与 /llm status 相同") \
    row("/llm setup deepseek <api_token> [model]", "Apply the DeepSeek preset", "使用 DeepSeek 预设，模型名称可省略") \
    row("/llm setup openai <api_token> [model]", "Apply the OpenAI preset", "使用 OpenAI 预设，模型名称可省略") \
    row("/llm setup qwen <api_token> [model]", "Apply the Qwen preset", "使用通义千问预设，模型名称可省略") \
    row("/llm setup anthropic <api_token> [model]", "Apply the Anthropic preset", "使用 Anthropic 预设，模型名称可省略") \
    row(LLM_CUSTOM_SETUP, "Configure a custom model service", "配置自定义协议、API 地址、模型名称和密钥") \
    row("/llm token <api_token>", "Set the API key", "设置 API 密钥") \
    row("/llm model <model>", "Set the model name", "设置模型名称") \
    row("/llm backend <backend>", "Set the model API protocol", "设置模型 API 协议") \
    row("/llm base-url <base_url>", "Set the API base URL", "设置模型 API 地址") \
    row("/llm url <base_url>", "Alias for /llm base-url", "设置模型 API 地址，与 /llm base-url 相同") \
    row("/llm test", "Check whether configuration is complete", "检查模型配置是否完整") \
    row("/llm reset", "Clear model connection settings and API key", "清空模型连接配置和 API 密钥")
#define COMMAND_ROW_EN(command, en, zh) command "  " en "\n"
#define COMMAND_ROW_ZH(command, en, zh) command "  " zh "\n"

static const char s_usage_en[] = "Usage:\n" LLM_COMMAND_ROWS(COMMAND_ROW_EN);
static const char s_usage_zh[] =
    "模型配置指令\n"
    LLM_COMMAND_ROWS(COMMAND_ROW_ZH)
    "\n"
    "<...> 内是必填参数，[model] 表示模型名称可省略。";

static bool is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

static bool config_flag_enabled(const char *value)
{
    return strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
}

static const char *skip_space(const char *text)
{
    if (!text) {
        return "";
    }
    while (*text && is_space(*text)) {
        text++;
    }
    return text;
}

static esp_err_t read_command_input(const char *input_json, char *command, size_t command_size, bool *chinese)
{
    cJSON *root = NULL;
    cJSON *item = NULL;

    if (!command || command_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    command[0] = '\0';
    if (!input_json || !input_json[0]) {
        return ESP_OK;
    }
    root = cJSON_Parse(input_json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *language = cJSON_GetObjectItemCaseSensitive(root, "language");
    if (cJSON_IsString(language)) {
        if (strcmp(language->valuestring, "zh-cn") == 0) {
            *chinese = true;
        } else if (strcmp(language->valuestring, "en") == 0) {
            *chinese = false;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (cJSON_IsString(item) && item->valuestring) {
        if (strlcpy(command, item->valuestring, command_size) >= command_size) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t read_token(const char **cursor, char *token, size_t token_size, bool *out_has_token)
{
    const char *start;
    size_t len = 0;

    if (!cursor || !*cursor || !token || token_size == 0 || !out_has_token) {
        return ESP_ERR_INVALID_ARG;
    }
    token[0] = '\0';
    *out_has_token = false;
    start = skip_space(*cursor);
    if (!start[0]) {
        *cursor = start;
        return ESP_OK;
    }
    while (start[len] && !is_space(start[len])) {
        len++;
    }
    if (len >= token_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(token, start, len);
    token[len] = '\0';
    *cursor = start + len;
    *out_has_token = true;
    return ESP_OK;
}

static bool find_value_token(const char *command, const char *key, char *value, size_t value_size)
{
    const char *cursor = command;
    size_t key_len = strlen(key);
    char *token = NULL;
    bool has_token = false;
    bool found = false;

    if (!value || value_size == 0) {
        return false;
    }
    value[0] = '\0';
    token = calloc(1, CAP_LLM_CONFIG_STR_LEN);
    if (!token) {
        return false;
    }
    while (read_token(&cursor, token, CAP_LLM_CONFIG_STR_LEN, &has_token) == ESP_OK && has_token) {
        if (strncmp(token, key, key_len) == 0 && token[key_len] == '=') {
            strlcpy(value, token + key_len + 1, value_size);
            found = true;
            break;
        }
    }
    free(token);
    return found;
}

static const cap_llm_preset_t *find_preset(const char *id)
{
    for (size_t i = 0; i < sizeof(s_presets) / sizeof(s_presets[0]); i++) {
        if (strcmp(s_presets[i].id, id) == 0) {
            return &s_presets[i];
        }
    }
    return NULL;
}

static void mask_token(const char *token, bool chinese, char *out, size_t out_size)
{
    size_t len;

    if (!out || out_size == 0) {
        return;
    }
    if (!token || !token[0]) {
        strlcpy(out, chinese ? "未填写" : "(missing)", out_size);
        return;
    }
    len = strlen(token);
    if (len <= 8) {
        strlcpy(out, chinese ? "已配置" : "configured", out_size);
        return;
    }
    snprintf(out, out_size, "%.4s...%s", token, token + len - 4);
}

static void apply_preset(cap_llm_config_t *config, const cap_llm_preset_t *preset)
{
    strlcpy(config->backend_type, preset->backend_type, sizeof(config->backend_type));
    strlcpy(config->base_url, preset->base_url, sizeof(config->base_url));
    strlcpy(config->auth_type, preset->auth_type, sizeof(config->auth_type));
    strlcpy(config->model, preset->model, sizeof(config->model));
    strlcpy(config->max_tokens_field, preset->max_tokens_field, sizeof(config->max_tokens_field));
    strlcpy(config->default_image_max_bytes,
            preset->default_image_max_bytes,
            sizeof(config->default_image_max_bytes));
    strlcpy(config->supports_tools, preset->supports_tools, sizeof(config->supports_tools));
    strlcpy(config->supports_vision, preset->supports_vision, sizeof(config->supports_vision));
    strlcpy(config->image_remote_url_only,
            preset->image_remote_url_only,
            sizeof(config->image_remote_url_only));
}

static bool command_has_sensitive_token(const char *subcommand, const char *command)
{
    char *value = NULL;
    bool found = false;

    if (strcmp(subcommand, "token") == 0 || strcmp(subcommand, "setup") == 0) {
        return true;
    }

    value = calloc(1, CAP_LLM_CONFIG_STR_LEN);
    if (!value) {
        return false;
    }
    found = find_value_token(command, "token", value, CAP_LLM_CONFIG_STR_LEN) ||
            find_value_token(command, "key", value, CAP_LLM_CONFIG_STR_LEN) ||
            find_value_token(command, "api_key", value, CAP_LLM_CONFIG_STR_LEN) ||
            find_value_token(command, "api-token", value, CAP_LLM_CONFIG_STR_LEN);
    free(value);
    return found;
}

static bool context_is_obvious_group_chat(const claw_cap_call_context_t *ctx)
{
    const char *chat_id = ctx ? ctx->chat_id : NULL;

    if (!chat_id || !chat_id[0]) {
        return false;
    }
    return strstr(chat_id, "group") != NULL ||
           strstr(chat_id, "room") != NULL ||
           strstr(chat_id, "guild") != NULL;
}

static void write_status(const cap_llm_config_t *config, bool chinese, char *output, size_t output_size)
{
    char masked[32];

    mask_token(config->api_key, chinese, masked, sizeof(masked));
    const char *missing = chinese ? "未填写" : "(missing)";
    const char *tools = config->supports_tools[0] ? config->supports_tools : "false";
    const char *vision = config->supports_vision[0] ? config->supports_vision : "false";
    if (chinese) {
        tools = config_flag_enabled(tools) ? "开启" : "关闭";
        vision = config_flag_enabled(vision) ? "开启" : "关闭";
    }
    snprintf(output,
             output_size,
             chinese ?
             "模型配置状态\n"
             "协议 %s\n"
             "API 地址 %s\n"
             "模型 %s\n"
             "API 密钥 %s\n"
             "工具调用 %s · 图片输入 %s" :
             "LLM status\n"
             "backend: %s\n"
             "base_url: %s\n"
             "model: %s\n"
             "token: %s\n"
             "tools: %s vision: %s",
             config->backend_type[0] ? config->backend_type : missing,
             config->base_url[0] ? config->base_url : missing,
             config->model[0] ? config->model : missing,
             masked,
             tools, vision);
}

static bool config_ready(const cap_llm_config_t *config)
{
    return config->api_key[0] &&
           config->backend_type[0] &&
           config->base_url[0] &&
           config->model[0];
}

static esp_err_t save_config(cap_llm_config_t *config, bool chinese, char *output, size_t output_size)
{
    esp_err_t err;

    if (!s_provider.apply_config) {
        strlcpy(output, chinese ? "模型配置尚未就绪，无法保存。" :
                "LLM config save failed: provider is not configured.", output_size);
        return ESP_ERR_INVALID_STATE;
    }

    err = s_provider.apply_config(config, s_provider.user_ctx);

    if (err != ESP_OK) {
        snprintf(output, output_size, chinese ? "保存模型配置失败（%s）。" :
                 "LLM config save failed: %s", esp_err_to_name(err));
        return err;
    }
    bool ready = config_ready(config);
    if (chinese) {
        strlcpy(output, ready ? "已保存模型配置。下一条消息将使用此模型。" :
                "已保存模型配置。部分必填项仍为空，可发送 /llm status 查看。", output_size);
    } else {
        strlcpy(output, ready ? "LLM config saved. The next message will use this model." :
                "LLM config saved. Some required fields are still missing; run /llm status.", output_size);
    }
    return ESP_OK;
}

static esp_err_t llm_config_execute(const char *input_json,
                                    const claw_cap_call_context_t *ctx,
                                    char *output,
                                    size_t output_size)
{
    cap_llm_config_t *config = NULL;
    cap_llm_config_parse_buf_t *parse_buf = NULL;
    char *command = NULL;
    const char *cursor = NULL;
    char subcommand[32];
    bool has_subcommand = false;
    bool chat_command = ctx && ctx->source_cap && strcmp(ctx->source_cap, "claw_event_router") == 0 &&
                        ctx->chat_id && ctx->chat_id[0];
    bool chinese = chat_command;
    esp_err_t err = ESP_OK;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    config = calloc(1, sizeof(*config));
    parse_buf = calloc(1, sizeof(*parse_buf));
    command = calloc(1, 512);
    if (!config || !parse_buf || !command) {
        err = ESP_ERR_NO_MEM;
        strlcpy(output, chinese ? "设备内存不足，无法执行模型配置指令。" :
                "LLM command failed: out of memory.", output_size);
        goto cleanup;
    }

    err = read_command_input(input_json, command, 512, &chinese);
    const char *usage = chinese ? s_usage_zh : s_usage_en;
    if (err != ESP_OK) {
        strlcpy(output, chinese ? "/llm 参数无效，请发送 /llm help 查看用法。" :
                "Invalid /llm command input.", output_size);
        goto cleanup;
    }
    cursor = command;
    err = read_token(&cursor, subcommand, sizeof(subcommand), &has_subcommand);
    if (err != ESP_OK || !has_subcommand ||
            strcmp(subcommand, "help") == 0 || strcmp(subcommand, "?") == 0) {
        strlcpy(output, usage, output_size);
        err = ESP_OK;
        goto cleanup;
    }

    if (!s_provider.get_config) {
        strlcpy(output, chinese ? "模型配置尚未就绪。" :
                "LLM config is not ready: provider is not configured.", output_size);
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    err = s_provider.get_config(config, s_provider.user_ctx);
    if (err != ESP_OK) {
        snprintf(output, output_size, chinese ? "无法读取模型配置（%s）。" :
                 "LLM config is not ready: %s", esp_err_to_name(err));
        goto cleanup;
    }

    if (strcmp(subcommand, "status") == 0 || strcmp(subcommand, "show") == 0) {
        write_status(config, chinese, output, output_size);
        err = ESP_OK;
        goto cleanup;
    }

    if (command_has_sensitive_token(subcommand, command) && context_is_obvious_group_chat(ctx)) {
        strlcpy(output, chinese ? "请在私聊、在线聊天或设备设置页填写 API 密钥。" :
                "For safety, configure API tokens in a private chat, Web IM, or the device settings page.",
                output_size);
        err = ESP_OK;
        goto cleanup;
    }

    if (strcmp(subcommand, "setup") == 0) {
        bool has_provider = false;
        bool has_token = false;
        bool has_model = false;

        err = read_token(&cursor, parse_buf->provider, sizeof(parse_buf->provider), &has_provider);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "setup provider parse failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        if (!has_provider) {
            strlcpy(output, usage, output_size);
            err = ESP_OK;
            goto cleanup;
        }
        if (strcmp(parse_buf->provider, "custom") == 0) {
            bool has_backend = find_value_token(command,
                                                "backend",
                                                parse_buf->backend,
                                                sizeof(parse_buf->backend));
            bool has_url = find_value_token(command, "url", parse_buf->url, sizeof(parse_buf->url));
            bool has_base_url = find_value_token(command,
                                                 "base_url",
                                                 parse_buf->base_url,
                                                 sizeof(parse_buf->base_url));
            bool has_model = find_value_token(command,
                                              "model",
                                              parse_buf->custom_model,
                                              sizeof(parse_buf->custom_model));
            bool has_token = find_value_token(command,
                                              "token",
                                              parse_buf->custom_token,
                                              sizeof(parse_buf->custom_token));

            if (!has_token) {
                has_token = find_value_token(command,
                                             "key",
                                             parse_buf->custom_token,
                                             sizeof(parse_buf->custom_token));
            }
            if (!has_token) {
                has_token = find_value_token(command,
                                             "api_key",
                                             parse_buf->custom_token,
                                             sizeof(parse_buf->custom_token));
            }
            if (!has_token) {
                has_token = find_value_token(command,
                                             "api-token",
                                             parse_buf->custom_token,
                                             sizeof(parse_buf->custom_token));
            }
            if (!has_backend || !(has_url || has_base_url) || !has_model || !has_token) {
                strlcpy(output, chinese ?
                        "请填写协议、地址、模型名称和 API 密钥。\n" LLM_CUSTOM_SETUP :
                        "Usage: " LLM_CUSTOM_SETUP,
                        output_size);
                err = ESP_OK;
                goto cleanup;
            }
            strlcpy(config->backend_type, parse_buf->backend, sizeof(config->backend_type));
            strlcpy(config->base_url,
                    has_url ? parse_buf->url : parse_buf->base_url,
                    sizeof(config->base_url));
            strlcpy(config->model, parse_buf->custom_model, sizeof(config->model));
            strlcpy(config->api_key, parse_buf->custom_token, sizeof(config->api_key));
            if (!config->auth_type[0]) {
                strlcpy(config->auth_type, "bearer", sizeof(config->auth_type));
            }
            if (!config->max_tokens_field[0]) {
                strlcpy(config->max_tokens_field, "max_tokens", sizeof(config->max_tokens_field));
            }
            if (!config->default_image_max_bytes[0]) {
                strlcpy(config->default_image_max_bytes, "524288",
                        sizeof(config->default_image_max_bytes));
            }
            err = save_config(config, chinese, output, output_size);
            goto cleanup;
        }

        const cap_llm_preset_t *preset = find_preset(parse_buf->provider);
        if (!preset) {
            snprintf(output,
                     output_size,
                     chinese ? "未知的模型服务商 %s。\n\n%s" : "Unknown LLM provider: %s\n\n%s",
                     parse_buf->provider,
                     usage);
            err = ESP_OK;
            goto cleanup;
        }
        err = read_token(&cursor, parse_buf->token, sizeof(parse_buf->token), &has_token);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "setup token parse failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        err = read_token(&cursor, parse_buf->model, sizeof(parse_buf->model), &has_model);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "setup model parse failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        if (!has_token) {
            snprintf(output, output_size, chinese ? "缺少 API 密钥。\n\n%s" : "Missing API token.\n\n%s",
                     usage);
            err = ESP_OK;
            goto cleanup;
        }
        apply_preset(config, preset);
        strlcpy(config->api_key, parse_buf->token, sizeof(config->api_key));
        if (has_model) {
            strlcpy(config->model, parse_buf->model, sizeof(config->model));
        }
        err = save_config(config, chinese, output, output_size);
        goto cleanup;
    }

    if (strcmp(subcommand, "token") == 0) {
        bool has_token = false;

        err = read_token(&cursor, parse_buf->token, sizeof(parse_buf->token), &has_token);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "token parse failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        if (!has_token) {
            strlcpy(output, chinese ? "请填写 API 密钥。\n/llm token <api_token>" :
                    "Usage: /llm token <api_token>", output_size);
            err = ESP_OK;
            goto cleanup;
        }
        strlcpy(config->api_key, parse_buf->token, sizeof(config->api_key));
        err = save_config(config, chinese, output, output_size);
        goto cleanup;
    }

    if (strcmp(subcommand, "model") == 0) {
        bool has_model = false;

        err = read_token(&cursor, parse_buf->model, sizeof(parse_buf->model), &has_model);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "model parse failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        if (!has_model) {
            strlcpy(output, chinese ? "请填写模型名称。\n/llm model <model>" :
                    "Usage: /llm model <model>", output_size);
            err = ESP_OK;
            goto cleanup;
        }
        strlcpy(config->model, parse_buf->model, sizeof(config->model));
        err = save_config(config, chinese, output, output_size);
        goto cleanup;
    }

    if (strcmp(subcommand, "backend") == 0) {
        bool has_backend = false;

        err = read_token(&cursor, parse_buf->backend, sizeof(parse_buf->backend), &has_backend);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "backend parse failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        if (!has_backend) {
            strlcpy(output, chinese ?
                    "请选择模型 API 协议。\n/llm backend <openai_compatible|anthropic_compatible>" :
                    "Usage: /llm backend <openai_compatible|anthropic_compatible>", output_size);
            err = ESP_OK;
            goto cleanup;
        }
        strlcpy(config->backend_type, parse_buf->backend, sizeof(config->backend_type));
        err = save_config(config, chinese, output, output_size);
        goto cleanup;
    }

    if (strcmp(subcommand, "base-url") == 0 || strcmp(subcommand, "url") == 0) {
        bool has_url = false;

        err = read_token(&cursor, parse_buf->url, sizeof(parse_buf->url), &has_url);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "base-url parse failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        if (!has_url) {
            strlcpy(output, chinese ? "请填写模型 API 地址。\n/llm base-url <base_url>" :
                    "Usage: /llm base-url <base_url>", output_size);
            err = ESP_OK;
            goto cleanup;
        }
        strlcpy(config->base_url, parse_buf->url, sizeof(config->base_url));
        err = save_config(config, chinese, output, output_size);
        goto cleanup;
    }

    if (strcmp(subcommand, "test") == 0) {
        bool ready = config_ready(config);
        if (chinese) {
            strlcpy(output, ready ? "模型配置已填全，发送一条普通消息即可测试。" :
                    "模型配置还未填全，可发送 /llm status 查看。", output_size);
        } else {
            strlcpy(output, ready ? "LLM config has all required fields. Send a normal message to test the model." :
                    "LLM config is incomplete. Run /llm status.", output_size);
        }
        err = ESP_OK;
        goto cleanup;
    }

    if (strcmp(subcommand, "reset") == 0) {
        config->api_key[0] = '\0';
        config->backend_type[0] = '\0';
        config->model[0] = '\0';
        config->base_url[0] = '\0';
        config->auth_type[0] = '\0';
        config->max_tokens_field[0] = '\0';
        config->supports_tools[0] = '\0';
        config->supports_vision[0] = '\0';
        config->image_remote_url_only[0] = '\0';
        err = save_config(config, chinese, output, output_size);
        goto cleanup;
    }

    strlcpy(output, usage, output_size);
    err = ESP_OK;

cleanup:
    if (err != ESP_OK && !output[0]) {
        strlcpy(output, chinese ? "/llm 参数无效或过长，请发送 /llm help 查看用法。" :
                "Invalid or too long /llm argument. Run /llm help.", output_size);
    }
    free(command);
    free(parse_buf);
    free(config);
    /* The router sends the command's text reply after a successful cap call. */
    return chat_command && output[0] ? ESP_OK : err;
}

static const claw_cap_descriptor_t s_llm_config_caps[] = {
    {
        .id = "llm_config_command",
        .name = "llm_config_command",
        .family = "app",
        .description = "Handle /llm configuration commands.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
                             "\"language\":{\"type\":\"string\",\"enum\":[\"en\",\"zh-cn\"]}}}",
        .execute = llm_config_execute,
    },
};

static const claw_cap_group_t s_llm_config_group = {
    .group_id = "cap_llm_config",
    .plugin_name = "cap_llm_config",
    .version = "1.0.0",
    .descriptors = s_llm_config_caps,
    .descriptor_count = sizeof(s_llm_config_caps) / sizeof(s_llm_config_caps[0]),
};

esp_err_t cap_llm_config_set_provider(const cap_llm_config_provider_t *provider)
{
    if (!provider || !provider->get_config || !provider->apply_config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_provider = *provider;
    return ESP_OK;
}

esp_err_t cap_llm_config_register_group(void)
{
    if (!s_provider.get_config || !s_provider.apply_config) {
        return ESP_ERR_INVALID_STATE;
    }
    if (claw_cap_group_exists(s_llm_config_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_llm_config_group);
}
