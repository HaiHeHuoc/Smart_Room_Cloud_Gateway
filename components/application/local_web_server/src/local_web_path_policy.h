#pragma once

#include <stddef.h>

#include "esp_err.h"

#define LOCAL_WEB_LOGICAL_PATH_MAX_LEN 192U
#define LOCAL_WEB_LOGICAL_COMPONENT_MAX_LEN 64U

/**
 * Decode once and normalize a browser-supplied path into a logical path below
 * the Web-visible root. The output always starts with `/`; raw VFS paths,
 * traversal components, duplicate/trailing separators, controls, backslashes,
 * malformed percent escapes, and overlong path components are rejected.
 */
esp_err_t local_web_path_policy_normalize(
    const char *encoded_path,
    char *logical_path,
    size_t logical_path_size);
