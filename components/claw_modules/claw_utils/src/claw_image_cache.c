/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_image_cache.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#define CACHE_FREE_RESERVE (64 * 1024)
#define CACHE_STREAM_RESERVE (32 * 1024)
#define CACHE_PATH_MAX 256
#define CACHE_PART_SUFFIX ".clawpart"
#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef struct {
    char *path;
    size_t size;
    time_t modified;
} cache_file_t;

typedef struct {
    cache_file_t *files;
    size_t count;
    size_t capacity;
    uint64_t bytes;
    size_t kept_bytes;
    const char *keep;
    bool clean_parts;
} cache_scan_t;

static const char *const s_platforms[] = {"qq", "telegram", "feishu", "wechat", "webim"};
static char s_storage[128];
static char s_inbox[sizeof(s_storage) + sizeof("/inbox")];
static SemaphoreHandle_t s_lock;
static size_t s_inflight;
static size_t s_pending;

static bool cache_name(const char *name, const char *platform, bool web)
{
    if (web) {
        size_t length = strspn(name, "0123456789abcdefghijklmnopqrstuvwxyz");
        return length >= 8 && length <= 12 && name[length] == '_';
    }
    size_t length = strlen(platform);
    if (strncmp(name, platform, length) != 0 || name[length] != '_') {
        return false;
    }
    name += length + 1;
    return strspn(name, "0123456789abcdef") == 8 && name[8] == '_';
}

static bool cache_image_name(const char *name)
{
    const char *ext = strrchr(name, '.');
    static const char *const extensions[] = {
        ".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".avif", ".heic", ".heif", ".tiff", ".tif", ".svg"
    };
    if (!ext) {
        return false;
    }
    for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); ++i) {
        if (strcasecmp(ext, extensions[i]) == 0) {
            return true;
        }
    }
    return strcmp(ext, ".bin") == 0 && (strstr(name, "_image.") || strstr(name, "_photo."));
}

static void cache_remove_empty_chat(char *path)
{
    char *end = strrchr(path, '/');
    *end = '\0';
    if (strchr(path + strlen(s_inbox) + 1, '/')) {
        (void)rmdir(path);
    }
    *end = '/';
}

bool claw_image_cache_manages(const char *path)
{
    size_t root_len = strlen(s_inbox);
    if (!s_inbox[0] || !path || strncmp(path, s_inbox, root_len) != 0 || path[root_len] != '/' ||
            strstr(path, "..") || strchr(path, '\\')) {
        return false;
    }
    const char *platform = path + root_len + 1;
    const char *slash = strchr(platform, '/');
    if (!slash) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_platforms) / sizeof(*s_platforms); ++i) {
        const char *kind = s_platforms[i];
        if ((size_t)(slash - platform) != strlen(kind) || strncmp(platform, kind, slash - platform) != 0) {
            continue;
        }
        bool web = strcmp(kind, "webim") == 0;
        const char *name = slash + 1;
        if (!web) {
            slash = strchr(name, '/');
            if (!slash || slash == name) {
                return false;
            }
            name = slash + 1;
        }
        return !strchr(name, '/') && cache_name(name, kind, web) && cache_image_name(name);
    }
    return false;
}

static esp_err_t cache_scan_directory(char *path, const char *platform, bool chats, cache_scan_t *scan)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    size_t base = strlen(path);
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || strchr(entry->d_name, '/')) {
            continue;
        }
        int n = snprintf(path + base, CACHE_PATH_MAX - base, "/%s", entry->d_name);
        if (n < 0 || (size_t)n >= CACHE_PATH_MAX - base) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (chats && S_ISDIR(st.st_mode)) {
            err = cache_scan_directory(path, platform, false, scan);
            if (err != ESP_OK) {
                break;
            }
            if (scan->clean_parts) {
                (void)rmdir(path);
            }
        } else if (!chats && S_ISREG(st.st_mode) && cache_name(entry->d_name, platform, strcmp(platform, "webim") == 0)) {
            const char *ext = strrchr(entry->d_name, '.');
            if (scan->clean_parts && ext && strcmp(ext, CACHE_PART_SUFFIX) == 0) {
                (void)unlink(path);
                continue;
            }
            if (!claw_image_cache_manages(path)) {
                continue;
            }
            scan->bytes += st.st_size;
            if (scan->keep && strcasecmp(scan->keep, path) == 0) {
                scan->kept_bytes = st.st_size;
                continue;
            }
            if (scan->count == scan->capacity) {
                size_t next = scan->capacity + 16;
                cache_file_t *files = realloc(scan->files, next * sizeof(*files));
                if (!files) {
                    err = ESP_ERR_NO_MEM;
                    break;
                }
                scan->files = files;
                scan->capacity = next;
            }
            char *copy = strdup(path);
            if (!copy) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            scan->files[scan->count++] = (cache_file_t){copy, st.st_size, st.st_mtime};
        }
    }
    path[base] = '\0';
    closedir(dir);
    return err;
}

static int cache_compare(const void *left, const void *right)
{
    const cache_file_t *a = left;
    const cache_file_t *b = right;
    if (a->modified == b->modified) {
        return strcmp(a->path, b->path);
    }
    return a->modified < b->modified ? -1 : 1;
}

/* Called with the cache lock held. Pending bytes have not been written to disk. */
static esp_err_t cache_make_room(size_t bytes, const char *keep, bool clean_parts, size_t *spare)
{
    cache_scan_t scan = {.keep = keep, .clean_parts = clean_parts};
    char *path = malloc(CACHE_PATH_MAX);
    if (!path) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(s_platforms) / sizeof(*s_platforms); ++i) {
        const char *platform = s_platforms[i];
        snprintf(path, CACHE_PATH_MAX, "%s/%s", s_inbox, platform);
        err = cache_scan_directory(path, platform, strcmp(platform, "webim") != 0, &scan);
        if (err != ESP_OK) {
            break;
        }
    }
    free(path);

    if (err == ESP_OK && scan.count > 1) {
        qsort(scan.files, scan.count, sizeof(*scan.files), cache_compare);
    }
    size_t next = 0;
    bool full = false;
    if (scan.kept_bytes + s_inflight + bytes > CLAW_IMAGE_CACHE_LIMIT) {
        err = ESP_ERR_NO_MEM;
        full = true;
    }
    while (err == ESP_OK) {
        uint64_t free_bytes = 0;
        err = esp_vfs_fat_info(s_storage, NULL, &free_bytes);
        if (err != ESP_OK) {
            break;
        }
        uint64_t quota_used = scan.bytes + s_inflight + bytes;
        size_t floor = CACHE_FREE_RESERVE + s_pending + bytes;
        if (quota_used <= CLAW_IMAGE_CACHE_LIMIT && free_bytes >= floor) {
            if (spare) {
                size_t quota_left = CLAW_IMAGE_CACHE_LIMIT - quota_used;
                uint64_t disk_left = free_bytes - floor;
                *spare = disk_left < quota_left ? (size_t)disk_left : quota_left;
            }
            break;
        }
        if (CONFIG_FATFS_FS_LOCK == 0 || next == scan.count) {
            err = ESP_ERR_NO_MEM;
            full = true;
            break;
        }
        cache_file_t *file = &scan.files[next++];
        if (unlink(file->path) == 0) {
            scan.bytes -= file->size;
            cache_remove_empty_chat(file->path);
        }
    }
    for (size_t i = 0; i < scan.count; ++i) {
        free(scan.files[i].path);
    }
    free(scan.files);
    if (full) {
        errno = ENOSPC;
    }
    return err;
}

esp_err_t claw_image_cache_init(const char *storage_root)
{
    if (!storage_root || storage_root[0] != '/' || strlen(storage_root) >= sizeof(s_storage)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_storage, storage_root, sizeof(s_storage));
    snprintf(s_inbox, sizeof(s_inbox), "%s/inbox", s_storage);
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* No writers exist at mount time, so all temporary attachment files are stale. */
    (void)cache_make_room(0, NULL, true, NULL);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t cache_create_parents(char *path)
{
    char *last = strrchr(path, '/');
    if (!last || last == path) {
        return ESP_OK;
    }
    char *start = last;
    struct stat st;
    for (char *slash = last; slash != path;) {
        *slash = '\0';
        bool found = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        *slash = '/';
        start = slash;
        if (found) {
            start++;
            break;
        }
        while (--slash != path && *slash != '/') {}
    }
    for (char *slash = start; slash <= last; ++slash) {
        if (*slash == '/') {
            *slash = '\0';
            int rc = mkdir(path, 0755);
            *slash = '/';
            if (rc != 0 && errno != EEXIST) {
                return ESP_FAIL;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t cache_reserve(claw_image_cache_writer_t *writer, size_t end)
{
    if (!writer->cached_image || end <= writer->reserved) {
        return ESP_OK;
    }
    size_t extra = end - writer->reserved;
    size_t spare = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = cache_make_room(extra, writer->path, false, &spare);
    if (err == ESP_OK) {
        if (!writer->expected) {
            extra += spare < CACHE_STREAM_RESERVE ? spare : CACHE_STREAM_RESERVE;
        }
        writer->reserved += extra;
        s_pending += extra;
        s_inflight += extra;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t claw_image_cache_begin(claw_image_cache_writer_t *writer, const char *path, size_t expected, size_t limit)
{
    errno = 0;
    if (!writer || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    *writer = (claw_image_cache_writer_t){.path = path, .expected = expected,
                                        .limit = limit ? limit : SIZE_MAX,
                                        .cached_image = claw_image_cache_manages(path)};
    if (writer->cached_image && !s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (writer->cached_image && writer->limit > CLAW_IMAGE_CACHE_LIMIT) {
        writer->limit = CLAW_IMAGE_CACHE_LIMIT;
    }
    if (expected > writer->limit) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (writer->cached_image && expected) {
        struct stat st;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool exists = stat(path, &st) == 0;
        if (exists && S_ISREG(st.st_mode) && (size_t)st.st_size == expected) {
            writer->file = fopen(path, "rb");
        }
        xSemaphoreGive(s_lock);
        if (exists) {
            return writer->file ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
    }
    size_t len = strlen(path) + sizeof(CACHE_PART_SUFFIX);
    writer->partial = malloc(len);
    if (!writer->partial) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(writer->partial, len, "%s" CACHE_PART_SUFFIX, path);
    esp_err_t err = cache_reserve(writer, expected ? expected : 1);
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (err == ESP_OK) {
        errno = 0;
        err = cache_create_parents(writer->partial);
    }
    if (err == ESP_OK) {
        int fd = open(writer->partial, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0600);
        if (fd >= 0) {
            writer->file = fdopen(fd, "wb");
            if (!writer->file) {
                int saved = errno;
                close(fd);
                (void)unlink(writer->partial);
                errno = saved;
            }
        }
        if (!writer->file) {
            err = ESP_FAIL;
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (err != ESP_OK) {
        int saved = errno;
        (void)claw_image_cache_finish(writer, false);
        errno = saved;
        return err;
    }
    /* Reflect writes in the free-space measurement before releasing reservations. */
    if (setvbuf(writer->file, NULL, _IONBF, 0) != 0) {
        (void)claw_image_cache_finish(writer, false);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t claw_image_cache_write(claw_image_cache_writer_t *writer, const void *data, size_t size)
{
    if (size > writer->limit - writer->bytes ||
            (writer->expected && size > writer->expected - writer->bytes)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!writer->partial) {
        /* Count the body of a replay without rewriting its protected cache file. */
        writer->bytes += size;
        return ESP_OK;
    }
    esp_err_t err = cache_reserve(writer, writer->bytes + size);
    if (err != ESP_OK) {
        return err;
    }
    errno = 0;
    if (fwrite(data, 1, size, writer->file) != size) {
        return ESP_FAIL;
    }
    writer->bytes += size;
    if (writer->cached_image) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_pending -= size;
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

esp_err_t claw_image_cache_finish(claw_image_cache_writer_t *writer, bool publish)
{
    esp_err_t err = ESP_OK;
    int saved = errno;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (writer->file) {
        if (fclose(writer->file) != 0) {
            saved = errno;
            err = ESP_FAIL;
        }
        if (publish && err == ESP_OK && (writer->bytes || !writer->cached_image) &&
                (!writer->expected || writer->expected == writer->bytes)) {
            if (writer->partial) {
                struct stat existing;
                if (stat(writer->path, &existing) == 0) {
                    if (!S_ISREG(existing.st_mode) || (size_t)existing.st_size != writer->bytes) {
                        err = ESP_ERR_INVALID_STATE;
                    }
                } else if (rename(writer->partial, writer->path) != 0) {
                    saved = errno;
                    err = ESP_FAIL;
                }
            }
        } else if (publish && err == ESP_OK) {
            err = ESP_ERR_INVALID_SIZE;
        }
        if (writer->partial && unlink(writer->partial) == 0 && writer->cached_image) {
            cache_remove_empty_chat(writer->partial);
        }
    }
    if (writer->reserved) {
        s_pending -= writer->reserved - writer->bytes;
        s_inflight -= writer->reserved;
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    free(writer->partial);
    writer->partial = NULL;
    writer->file = NULL;
    writer->reserved = 0;
    writer->cached_image = false;
    errno = saved;
    return err;
}
