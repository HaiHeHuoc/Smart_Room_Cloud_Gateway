#pragma once

/**
 * Return the fixed logical SD path for one presentation icon, or NULL when the
 * untrusted query token is not in the Web UI allowlist.
 */
const char *local_web_icon_logical_path(const char *name);
