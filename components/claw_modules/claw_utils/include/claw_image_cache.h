/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

#define CLAW_IMAGE_CACHE_LIMIT (1024 * 1024)

typedef struct {
    FILE *file;
    char *partial;
    const char *path;
    size_t expected;
    size_t limit;
    size_t bytes;
    size_t reserved;
    bool cached_image;
} claw_image_cache_writer_t;

/** Initialize once after mounting writable storage, before starting IM channels. */
esp_err_t claw_image_cache_init(const char *storage_root);
bool claw_image_cache_manages(const char *path);

/** An expected size of zero means the length is unknown. Keep path valid until finish. */
esp_err_t claw_image_cache_begin(claw_image_cache_writer_t *writer, const char *path, size_t expected, size_t limit);
esp_err_t claw_image_cache_write(claw_image_cache_writer_t *writer, const void *data, size_t size);
/** Publish a complete file or remove the temporary file on any error. */
esp_err_t claw_image_cache_finish(claw_image_cache_writer_t *writer, bool publish);
