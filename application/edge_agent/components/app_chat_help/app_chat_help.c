/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_chat_help.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_cap.h"
#include "claw_event_router.h"
#include "esp_log.h"

#include "device_status.h"
#include "token_usage.h"

#define HELP_COMMAND       "/help"
#define HELP_REPLY_TITLE   "ESP-Claw /help\n"
#define HELP_PAGE_SIZE     1400

static const char *TAG = "app_chat_help";
static atomic_bool s_registered;

typedef struct {
    const char *command;
    const char *description;
    const char *channel;
    bool builtin;
} help_command_t;

typedef bool (*help_command_visitor_t)(const help_command_t *command, void *ctx);

typedef struct {
    const char *name;
    const char *zh;
    const char *en;
} command_description_t;

/* Upstream routes still determine which commands are available. These labels
 * use command names so changes to argument placeholders need no translation. */
static const command_description_t s_descriptions[] = {
    {HELP_COMMAND, "查看设备状态和指令", "Show device status and commands"},
    {"/session", "查看会话指令用法", NULL},
    {"/session new", "创建并切换到新会话", NULL},
    {"/session list", "列出此聊天的会话", NULL},
    {"/session switch", "切换到指定会话", NULL},
    {"/session delete", "删除指定的非当前会话", NULL},
    {"/llm", "查看模型配置指令", NULL},
    {"/llm help", "查看模型配置指令", NULL},
    {"/llm ?", "查看模型配置指令", NULL},
    {"/llm status", "查看当前模型配置", "Show the current model configuration"},
    {"/llm show", "查看当前模型配置", NULL},
    {"/llm setup deepseek", "使用 DeepSeek 预设", "Apply the DeepSeek preset"},
    {"/llm setup openai", "使用 OpenAI 预设", "Apply the OpenAI preset"},
    {"/llm setup qwen", "使用通义千问预设", "Apply the Qwen preset"},
    {"/llm setup anthropic", "使用 Anthropic 预设", "Apply the Anthropic preset"},
    {"/llm setup custom", "设置自定义模型服务", "Configure a custom model endpoint"},
    {"/llm token", "设置 API 密钥", "Set the API key"},
    {"/llm model", "设置模型名称", "Set the model name"},
    {"/llm backend", "设置模型 API 协议", "Set the model API protocol"},
    {"/llm base-url", "设置模型 API 地址", "Set the model API base URL"},
    {"/llm url", "设置模型 API 地址", NULL},
    {"/llm test", "检查配置是否填全", NULL},
    {"/llm reset", "清空模型连接配置和密钥", "Clear the model connection settings and API key"},
};

static const command_description_t *find_description(const help_command_t *command)
{
    if (!command->builtin) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_descriptions) / sizeof(s_descriptions[0]); ++i) {
        const command_description_t *entry = &s_descriptions[i];
        size_t length = strlen(entry->name);
        if (strncmp(command->command, entry->name, length) != 0) {
            continue;
        }
        const char *suffix = command->command + length;
        if (!suffix[0] || (suffix[0] == ' ' &&
                (suffix[1] == '<' || suffix[1] == '[' || strncmp(suffix + 1, "backend=", 8) == 0))) {
            return entry;
        }
    }
    return NULL;
}

static const char *describe_command(const help_command_t *command, bool chinese)
{
    const command_description_t *entry = find_description(command);
    if (entry && chinese) {
        return entry->zh;
    }
    if (command->description && command->description[0]) {
        return command->description;
    }
    if (entry && entry->en) {
        return entry->en;
    }
    return chinese ? "执行此设备的聊天指令" : "Run this device's chat command";
}

static bool command_cap_ready(const char *name)
{
    claw_cap_descriptor_info_t info;
    return claw_cap_get_descriptor_state(name, &info) == ESP_OK && info.state == CLAW_CAP_STATE_STARTED;
}

static bool rule_available(const claw_event_router_rule_t *rule, const claw_event_t *event)
{
    if (!rule->enabled || (rule->match.event_type[0] && strcmp(rule->match.event_type, "message") != 0) ||
            (rule->match.event_key[0] && strcmp(rule->match.event_key, "text") != 0) ||
            (rule->match.content_type[0] && strcmp(rule->match.content_type, "text") != 0) ||
            rule->match.text[0] != '/' ||
            (event && rule->match.channel[0] && strcmp(event->source_channel, rule->match.channel) != 0) ||
            (event && rule->match.source_cap[0] && strcmp(event->source_cap, rule->match.source_cap) != 0) ||
            (rule->match.chat_id[0] && (!event || strcmp(event->chat_id, rule->match.chat_id) != 0))) {
        return false;
    }
    for (size_t i = 0; i < rule->action_count; ++i) {
        const claw_event_router_action_t *action = &rule->actions[i];
        if (action->kind == CLAW_EVENT_ROUTER_ACTION_CALL_CAP && !action->fail_open &&
                !command_cap_ready(action->cap)) {
            return false;
        }
    }
    return true;
}

/* The existing command capabilities already return their usage for an empty
 * command. Read that usage so upstream additions show up in both help views. */
static bool visit_cap_usage(const char *cap, const char *prefix, const char *channel,
                            help_command_visitor_t visit, void *ctx)
{
    char *usage = calloc(1, 2048);
    if (!usage) {
        return false;
    }
    claw_cap_call_context_t call_ctx = { .caller = CLAW_CAP_CALLER_SYSTEM };
    if (claw_cap_call(cap, "{}", &call_ctx, usage, 2048) != ESP_OK) {
        free(usage);
        return false;
    }

    bool found = false;
    size_t prefix_len = strlen(prefix);
    char *line = strchr(usage, '\n');
    while (line && line[1] == '/') {
        ++line;
        char *next = strchr(line, '\n');
        if (next) {
            *next = '\0';
        }
        if (strncmp(line, prefix, prefix_len) != 0 || (line[prefix_len] && line[prefix_len] != ' ')) {
            break;
        }
        char *description = strstr(line, "  ");
        if (description) {
            *description = '\0';
            ++description;
            while (*description == ' ') {
                ++description;
            }
        }
        found = true;
        if (!visit(&(help_command_t) { .command = line, .description = description,
                                      .channel = channel, .builtin = true }, ctx)) {
            break;
        }
        line = next;
    }
    free(usage);
    return found;
}

static void visit_commands(const claw_event_t *event, help_command_visitor_t visit, void *ctx)
{
    if (!visit(&(help_command_t) { .command = HELP_COMMAND, .builtin = true }, ctx)) {
        return;
    }
    claw_event_router_rule_t *rules = NULL;
    size_t count = 0;
    if (claw_event_router_list_rules(&rules, &count) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const claw_event_router_rule_t *rule = &rules[i];
        if (!rule_available(rule, event) || strcmp(rule->match.text, HELP_COMMAND) == 0) {
            continue;
        }
        const claw_event_router_action_t *first = rule->actions;
        if (rule->action_count && first->kind == CLAW_EVENT_ROUTER_ACTION_CALL_CAP &&
                rule->match.text_match_rule == CLAW_EVENT_ROUTER_TEXT_MATCH_PREFIX &&
                ((strcmp(rule->match.text, "/session") == 0 && strcmp(first->cap, "session_command") == 0) ||
                 (strcmp(rule->match.text, "/llm") == 0 && strcmp(first->cap, "llm_config_command") == 0)) &&
                visit_cap_usage(first->cap, rule->match.text, rule->match.channel, visit, ctx)) {
            continue;
        }
        if (!visit(&(help_command_t) { .command = rule->match.text, .description = rule->description,
                                      .channel = rule->match.channel }, ctx)) {
            break;
        }
    }
    claw_event_router_free_rule_list(rules, count);
}

typedef struct {
    char *page;
    size_t used;
    const claw_event_t *event;
    esp_err_t status;
} help_text_t;

static bool append_help(help_text_t *out, const char *format, ...)
{
    size_t remaining = HELP_PAGE_SIZE - out->used;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out->page + out->used, remaining, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= remaining) {
        out->page[out->used] = '\0';
        return false;
    }
    out->used += written;
    return true;
}

static bool send_help_page(help_text_t *out)
{
    out->status = claw_event_router_reply(out->event, out->page);
    out->used = 0;
    out->page[0] = '\0';
    return out->status == ESP_OK;
}

static bool append_command(const help_command_t *item, void *ctx)
{
    help_text_t *out = ctx;
    const char *description = describe_command(item, true);
    if (append_help(out, "%s · %s\n", item->command, description)) {
        return true;
    }
    return send_help_page(out) && append_help(out, "%s · %s\n", item->command, description);
}

static void append_usage(help_text_t *out, const char *label, const app_token_period_t *period, bool available)
{
    if (!available || (!period->reported && period->unreported)) {
        append_help(out, "%s 未知", label);
    } else {
        append_help(out, "%s %" PRIu64, label, period->tokens);
    }
    if (available && period->unreported) {
        append_help(out, "（%" PRIu64 " 次未返回用量）", period->unreported);
    }
    append_help(out, "\n");
}

static esp_err_t send_help_reply(const claw_event_t *event)
{
    char *text = calloc(1, HELP_PAGE_SIZE);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    app_chat_help_status_t status;
    app_chat_help_get_status(&status);
    app_token_snapshot_t usage;
    app_token_usage_snapshot(status.date[0] ? &status.local_time : NULL, &usage);
    help_text_t out = { .page = text, .event = event };

    append_help(&out, HELP_REPLY_TITLE "日期 %s\n", status.date[0] ? status.date : "待同步");
    if (status.cpu_percent >= 0) {
        append_help(&out, "CPU %d%%", status.cpu_percent);
    } else {
        append_help(&out, "CPU 待采样");
    }
    append_help(&out, " · 内存 %zu/%zu KiB",
                (status.internal_total - status.internal_free) / 1024, status.internal_total / 1024);
    if (status.psram_total) {
        append_help(&out, " · PSRAM %zu/%zu KiB",
                    (status.psram_total - status.psram_free) / 1024, status.psram_total / 1024);
    }
    append_help(&out, "\n");
    append_usage(&out, "今日 token", &usage.today, usage.available);
    append_usage(&out, "本月 token", &usage.month, usage.available);
    if (usage.available && !usage.durable) {
        append_help(&out, "用量暂存于内存\n");
    }
    if (status.ip[0]) {
        append_help(&out, "管理地址 http://%s/\n", status.ip);
    } else {
        append_help(&out, "管理地址 未连接\n");
    }
    append_help(&out, "\n可用指令\n");
    visit_commands(event, append_command, &out);
    if (out.status == ESP_OK && out.used > 0) {
        send_help_page(&out);
    }
    free(text);
    return out.status;
}

static bool add_json_command(const help_command_t *command, void *ctx)
{
    cJSON *item = cJSON_CreateObject();
    if (!item || !cJSON_AddStringToObject(item, "command", command->command) ||
            !cJSON_AddStringToObject(item, "description", describe_command(command, false)) ||
            !cJSON_AddStringToObject(item, "description_zh", describe_command(command, true)) ||
            !cJSON_AddStringToObject(item, "channel", command->channel ? command->channel : "") ||
            !cJSON_AddItemToArray(ctx, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static void add_usage_json(cJSON *root, const char *key, const app_token_period_t *period)
{
    cJSON *item = cJSON_AddObjectToObject(root, key);
    cJSON_AddNumberToObject(item, "tokens", (double)period->tokens);
    cJSON_AddNumberToObject(item, "reported", (double)period->reported);
    cJSON_AddNumberToObject(item, "unreported", (double)period->unreported);
}

cJSON *app_chat_help_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *commands = cJSON_AddArrayToObject(root, "commands");
    if (!commands) {
        cJSON_Delete(root);
        return NULL;
    }
    app_chat_help_status_t status;
    app_chat_help_get_status(&status);
    app_token_snapshot_t usage;
    app_token_usage_snapshot(status.date[0] ? &status.local_time : NULL, &usage);

    bool ready = atomic_load(&s_registered);
    cJSON_AddBoolToObject(root, "ready", ready);
    cJSON_AddStringToObject(root, "date", status.date);
    cJSON_AddStringToObject(root, "ip", status.ip);
    if (status.cpu_percent < 0) {
        cJSON_AddNullToObject(root, "cpu_percent");
    } else {
        cJSON_AddNumberToObject(root, "cpu_percent", status.cpu_percent);
    }
    cJSON_AddNumberToObject(root, "internal_total", status.internal_total);
    cJSON_AddNumberToObject(root, "internal_free", status.internal_free);
    cJSON_AddNumberToObject(root, "psram_total", status.psram_total);
    cJSON_AddNumberToObject(root, "psram_free", status.psram_free);
    cJSON_AddBoolToObject(root, "usage_available", usage.available);
    cJSON_AddBoolToObject(root, "usage_durable", usage.durable);
    add_usage_json(root, "today", &usage.today);
    add_usage_json(root, "month", &usage.month);
    if (ready) {
        visit_commands(NULL, add_json_command, commands);
    }
    return root;
}

static bool handle_chat_help(const claw_event_t *event, esp_err_t *out_status)
{
    const char *text = event->text;
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    if (strncmp(text, HELP_COMMAND, sizeof(HELP_COMMAND) - 1) != 0) {
        return false;
    }
    text += sizeof(HELP_COMMAND) - 1;
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    if (*text) {
        return false;
    }

    *out_status = send_help_reply(event);
    return true;
}

bool app_chat_help_is_reply(const char *text)
{
    return text && strncmp(text, HELP_REPLY_TITLE, sizeof(HELP_REPLY_TITLE) - 1) == 0;
}

esp_err_t app_chat_help_init(void)
{
    esp_err_t err = app_chat_help_status_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CPU sampling unavailable: %s", esp_err_to_name(err));
    }
    return app_token_usage_init();
}

esp_err_t app_chat_help_register(void)
{
    esp_err_t err = claw_event_router_set_chat_command_handler(handle_chat_help);
    atomic_store(&s_registered, err == ESP_OK);
    return err;
}
