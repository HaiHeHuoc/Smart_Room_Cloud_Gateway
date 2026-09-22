#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "local_web_path_policy.h"

#define LOCAL_WEB_DOWNLOAD_CONTENT_DISPOSITION_MAX_LEN \
    (sizeof("attachment; filename=\"\"; filename*=UTF-8''") + \
     (LOCAL_WEB_LOGICAL_COMPONENT_MAX_LEN * 4U))

/**
 * @brief Build a safe Content-Disposition value for one normalized file path.
 *
 * The ASCII filename is a browser-compatible fallback. `filename*` preserves
 * the original UTF-8 bytes using RFC 5987 percent encoding.
 */
bool local_web_download_content_disposition(
    const char *logical_path,
    char *output,
    size_t output_size);
