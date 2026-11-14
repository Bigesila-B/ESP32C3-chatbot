/* SPDX-License-Identifier: Apache-2.0 */
#include "cap_micropython.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_paths.h"
#include "claw_task.h"
#include "claw_utils_string.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

#include "py/builtin.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objlist.h"
#include "py/objmodule.h"
#include "py/runtime.h"

enum {
    PYTHON_PATH_BYTES = 256,
    PYTHON_SOURCE_MAX_BYTES = 32 * 1024,
    PYTHON_ARGS_BYTES = 4096,
    PYTHON_OUTPUT_BYTES = 4096,
    PYTHON_HEAP_BYTES = 256 * 1024,
    PYTHON_TASK_STACK_BYTES = 16 * 1024,
    PYTHON_DEFAULT_TIMEOUT_MS = 1000,
    PYTHON_MAX_TIMEOUT_MS = 5000,
    PYTHON_TASK_GRACE_MS = 250,
    PYTHON_YIELD_INTERVAL_US = 10000,
};

typedef struct {
    char path[PYTHON_PATH_BYTES];
    char directory[PYTHON_PATH_BYTES];
    char args[PYTHON_ARGS_BYTES];
    char output[PYTHON_OUTPUT_BYTES];
    size_t output_len;
    bool truncated;
    esp_err_t result;
    int timeout_ms;
    int64_t deadline_us;
    int64_t next_yield_us;
    SemaphoreHandle_t done;
    atomic_uint owners;
} micropython_job_t;

// MicroPython's VM state is global. The worker owns it while this slot is taken.
static atomic_uint s_vm_busy;
static micropython_job_t *s_current_job;

static bool path_is_inside(const char *path, const char *directory)
{
    if (!directory) {
        return false;
    }
    size_t len = strlen(directory);
    return strncmp(path, directory, len) == 0 && path[len] == '/' && path[len + 1] != '\0';
}

static bool path_is_normal(const char *path)
{
    if (!path || path[0] != '/' || strpbrk(path, "\\:") || strlen(path) >= PYTHON_PATH_BYTES) {
        return false;
    }
    // FAT treats trailing dots or spaces specially. Accept unambiguous components.
    for (const char *part = path + 1; ; ) {
        size_t len = strcspn(part, "/");
        if (len == 0 || part[len - 1] == '.' || part[len - 1] == ' ') {
            return false;
        }
        if (!part[len]) {
            return true;
        }
        part += len + 1;
    }
}

static bool script_path_is_allowed(const char *path)
{
    if (!path_is_normal(path)) {
        return false;
    }
    size_t len = strlen(path);
    return len > 3 && strcmp(path + len - 3, ".py") == 0 &&
           (path_is_inside(path, claw_paths_get(CLAW_PATH_SYSTEM)) ||
            path_is_inside(path, claw_paths_get(CLAW_PATH_DATA)));
}

static void job_release(micropython_job_t *job)
{
    if (atomic_fetch_sub_explicit(&job->owners, 1, memory_order_acq_rel) == 1) {
        vSemaphoreDelete(job->done);
        free(job);
    }
}

// Port hooks run only on the worker, including during imports and exception reporting.
void cap_micropython_vm_poll(void)
{
    micropython_job_t *job = s_current_job;
    int64_t now = esp_timer_get_time();
    if (now >= job->deadline_us && nlr_get_abort()) {
        nlr_jump_abort();
    }
    if (now >= job->next_yield_us) {
        job->next_yield_us = now + PYTHON_YIELD_INTERVAL_US;
        vTaskDelay(1);
    }
}

void mp_hal_stdout_tx_strn_cooked(const char *text, size_t len)
{
    micropython_job_t *job = s_current_job;
    size_t available = sizeof(job->output) - job->output_len - 1;
    size_t copied = len < available ? len : available;
    for (size_t i = 0; i < copied; ++i) {
        job->output[job->output_len++] = text[i] ? text[i] : '?';
    }
    job->output[job->output_len] = '\0';
    job->truncated |= copied != len;
}

static mp_obj_t micropython_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    (void)n_args;
    (void)args;
    (void)kwargs;
    mp_raise_OSError(MP_EACCES);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, micropython_open);

mp_import_stat_t mp_import_stat(const char *path)
{
    struct stat info;
    if (!path_is_normal(path) || !path_is_inside(path, s_current_job->directory) ||
        stat(path, &info) != 0) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    if (S_ISREG(info.st_mode)) {
        return MP_IMPORT_STAT_FILE;
    }
    return S_ISDIR(info.st_mode) ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_NO_EXIST;
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
    const char *path = qstr_str(filename);
    struct stat info;
    if (!path_is_normal(path) || !path_is_inside(path, s_current_job->directory) ||
        stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        mp_raise_OSError_with_filename(MP_ENOENT, path);
    }
    if (info.st_size < 0 || info.st_size > PYTHON_SOURCE_MAX_BYTES) {
        mp_raise_OSError_with_filename(MP_EFBIG, path);
    }

    size_t length = (size_t)info.st_size;
    char *source = m_new(char, length + 1);
    FILE *file = fopen(path, "rb");
    if (!file) {
        mp_raise_OSError_with_filename(MP_EIO, path);
    }
    size_t read_len = fread(source, 1, length + 1, file);
    bool read_failed = ferror(file) || read_len != length;
    fclose(file);
    if (read_failed || memchr(source, '\0', length)) {
        mp_raise_OSError_with_filename(MP_EIO, path);
    }
    return mp_lexer_new_from_str_len(filename, source, length, length + 1);
}

void nlr_jump_fail(void *value)
{
    (void)value;
    abort();
}

static void run_script(const micropython_job_t *job)
{
    mp_obj_t filename = mp_obj_new_str(job->path, strlen(job->path));
    mp_store_global(MP_QSTR___file__, filename);
    mp_obj_list_append(mp_sys_argv, filename);
    mp_obj_t script_dir = mp_obj_new_str(job->directory, strlen(job->directory));
    mp_sys_path = mp_obj_new_list(1, &script_dir);

    mp_obj_t json = mp_module_get_builtin(MP_QSTR_json, true);
    mp_obj_t loads = mp_load_attr(json, MP_QSTR_loads);
    mp_obj_t args = mp_obj_new_str(job->args, strlen(job->args));
    mp_store_global(qstr_from_str("args"), mp_call_function_1(loads, args));

    qstr filename_qstr = qstr_from_str(job->path);
    mp_lexer_t *lexer = mp_lexer_new_from_file(filename_qstr);
    mp_parse_tree_t tree = mp_parse(lexer, MP_PARSE_FILE_INPUT);
    mp_obj_t script = mp_compile(&tree, filename_qstr, false);
    mp_call_function_0(script);
}

static esp_err_t run_vm(micropython_job_t *job, void *heap)
{
    memset(&mp_state_ctx, 0, sizeof(mp_state_ctx));
    uint8_t *stack_end = pxTaskGetStackStart(NULL) + PYTHON_TASK_STACK_BYTES;
    mp_cstack_init_with_top(stack_end, PYTHON_TASK_STACK_BYTES);
    gc_init(heap, (uint8_t *)heap + PYTHON_HEAP_BYTES);
    job->deadline_us = esp_timer_get_time() + job->timeout_ms * INT64_C(1000);
    job->next_yield_us = esp_timer_get_time() + PYTHON_YIELD_INTERVAL_US;

    nlr_buf_t trap;
    if (nlr_push(&trap) == 0) {
        nlr_set_abort(&trap);
        mp_init();
        run_script(job);
        nlr_pop();
        nlr_set_abort(NULL);
        mp_deinit();
        return esp_timer_get_time() >= job->deadline_us ? ESP_ERR_TIMEOUT : ESP_OK;
    }

    esp_err_t result = ESP_ERR_TIMEOUT;
    if (trap.ret_val) {
        nlr_buf_t print_trap;
        if (nlr_push(&print_trap) == 0) {
            nlr_set_abort(&print_trap);
            mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(trap.ret_val));
            nlr_pop();
            result = ESP_FAIL;
        } else if (print_trap.ret_val) {
            result = ESP_FAIL;
        }
    }
    nlr_set_abort(NULL);
    mp_deinit();
    return result;
}

static void micropython_worker(void *arg)
{
    micropython_job_t *job = arg;
    void *heap = heap_caps_malloc(PYTHON_HEAP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (heap) {
        s_current_job = job;
        job->result = run_vm(job, heap);
        s_current_job = NULL;
        free(heap);
    } else {
        job->result = ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_vm_busy, 0, memory_order_release);
    xSemaphoreGive(job->done);
    job_release(job);
    claw_task_delete(NULL);
}

static esp_err_t parse_job(const char *input_json, micropython_job_t *job,
                           char *output, size_t output_size)
{
    cJSON *input = cJSON_ParseWithOpts(input_json, NULL, true);
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(input, "path");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(input, "args");
    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(input, "timeout_ms");
    const char *error = NULL;
    if (!cJSON_IsObject(input) || !cJSON_IsString(path) || !script_path_is_allowed(path->valuestring)) {
        error = "path must be an absolute .py file on the SYSTEM or DATA filesystem";
    } else if (args && (!cJSON_IsObject(args) ||
                       !cJSON_PrintPreallocated((cJSON *)args, job->args, sizeof(job->args), false))) {
        error = "args must be a JSON object under 4 KiB";
    } else if (timeout && (!cJSON_IsNumber(timeout) || timeout->valueint < 1 ||
                          timeout->valueint > PYTHON_MAX_TIMEOUT_MS ||
                          timeout->valuedouble != timeout->valueint)) {
        error = "timeout_ms must be an integer from 1 to 5000";
    }
    if (error) {
        snprintf(output, output_size, "Error: %s", error);
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }
    strcpy(job->path, path->valuestring);
    size_t parent_len = strrchr(job->path, '/') - job->path;
    memcpy(job->directory, job->path, parent_len);
    if (!args) {
        strcpy(job->args, "{}");
    }
    job->timeout_ms = timeout ? timeout->valueint : PYTHON_DEFAULT_TIMEOUT_MS;
    cJSON_Delete(input);
    return ESP_OK;
}

static void write_result(const micropython_job_t *job, char *output, size_t output_size)
{
    const char *prefix = "";
    switch (job->result) {
    case ESP_ERR_NO_MEM:
        snprintf(output, output_size, "Error: MicroPython heap allocation failed");
        return;
    case ESP_ERR_TIMEOUT:
        snprintf(output, output_size, "Error: MicroPython script timed out");
        return;
    case ESP_FAIL:
        prefix = "Error: MicroPython exception\n";
        break;
    default:
        if (!job->output_len) {
            snprintf(output, output_size, "OK");
            return;
        }
        break;
    }

    const char suffix[] = "\n[output truncated]";
    snprintf(output, output_size, "%s", prefix);
    size_t used = strlen(output);
    size_t available = output_size - used - 1;
    bool truncated = job->truncated || job->output_len > available;
    bool append_suffix = truncated && available >= sizeof(suffix) - 1;
    size_t limit = append_suffix ? available - (sizeof(suffix) - 1) : available;
    size_t copied = claw_utils_utf8_prefix_len(job->output, limit);
    memcpy(output + used, job->output, copied);
    output[used + copied] = '\0';
    if (append_suffix) {
        memcpy(output + used + copied, suffix, sizeof(suffix));
    }
}

static esp_err_t python_run_script(const char *input_json, const claw_cap_call_context_t *ctx,
                                    char *output, size_t output_size)
{
    (void)ctx;
    if (!output || !output_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!input_json) {
        snprintf(output, output_size, "Error: expected a JSON object with a script path");
        return ESP_ERR_INVALID_ARG;
    }
    // The reference count is atomic. Keep it in internal RAM on ESP32-S3.
    micropython_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!job) {
        snprintf(output, output_size, "Error: MicroPython job allocation failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = parse_job(input_json, job, output, output_size);
    if (result != ESP_OK) {
        free(job);
        return result;
    }
    if (atomic_exchange_explicit(&s_vm_busy, 1, memory_order_acquire)) {
        free(job);
        snprintf(output, output_size, "Error: MicroPython is already running a script");
        return ESP_ERR_INVALID_STATE;
    }

    static const claw_task_config_t task = {
        .name = "cap_micropython",
        .stack_size = PYTHON_TASK_STACK_BYTES,
        .priority = tskIDLE_PRIORITY,
        .core_id = tskNO_AFFINITY,
        .stack_policy = CLAW_TASK_STACK_INTERNAL_ONLY,
    };
    job->done = xSemaphoreCreateBinary();
    atomic_init(&job->owners, 2);
    if (!job->done || claw_task_create(&task, micropython_worker, job, NULL) != pdPASS) {
        if (job->done) {
            vSemaphoreDelete(job->done);
        }
        free(job);
        atomic_store_explicit(&s_vm_busy, 0, memory_order_release);
        snprintf(output, output_size, "Error: MicroPython worker allocation failed");
        return ESP_ERR_NO_MEM;
    }

    // Native operations may finish after the caller's deadline. The worker retains
    // the job and VM slot until it returns, so a later call cannot reuse either.
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(job->timeout_ms + PYTHON_TASK_GRACE_MS)) == pdTRUE) {
        result = job->result;
        write_result(job, output, output_size);
    } else {
        result = ESP_ERR_TIMEOUT;
        snprintf(output, output_size, "Error: MicroPython script timed out");
    }
    job_release(job);
    return result;
}

esp_err_t cap_micropython_register_group(void)
{
    static const claw_cap_descriptor_t tool = {
        .id = "python_run_script",
        .name = "python_run_script",
        .family = "micropython",
        .description = "Run an on-device .py script with MicroPython and return its printed output.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{"
                             "\"path\":{\"type\":\"string\",\"description\":\"Absolute SYSTEM or DATA .py path\"},"
                             "\"args\":{\"type\":\"object\",\"description\":\"Available to the script as args\"},"
                             "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000}},"
                             "\"required\":[\"path\"],\"additionalProperties\":false}",
        .execute = python_run_script,
    };
    static const claw_cap_group_t group = {
        .group_id = "cap_micropython",
        .plugin_name = "MicroPython",
        .version = "1.0",
        .descriptors = &tool,
        .descriptor_count = 1,
    };
    return claw_cap_register_group(&group);
}
