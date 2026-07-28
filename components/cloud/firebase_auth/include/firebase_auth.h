#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Macros ------------------------------------------------------------------- */
/** Maximum destination size required when requesting a copied ID token. */
#define FIREBASE_AUTH_ID_TOKEN_BUFFER_SIZE    4096U

/* Type Definitions --------------------------------------------------------- */
/** @brief Firebase Authentication lifecycle and latest request outcome. */
typedef enum
{
    /** firebase_auth_init() has not completed. */
    FIREBASE_AUTH_STATE_UNINITIALIZED = 0,
    /** Configuration and mutex are ready; no request has run yet. */
    FIREBASE_AUTH_STATE_INITIALIZED,
    /** Email/Password sign-in is in progress. */
    FIREBASE_AUTH_STATE_SIGNING_IN,
    /** Secure Token refresh is in progress. */
    FIREBASE_AUTH_STATE_REFRESHING,
    /** A valid ID token is cached. */
    FIREBASE_AUTH_STATE_AUTHENTICATED,
    /** Credentials, UID validation, or token parsing failed. */
    FIREBASE_AUTH_STATE_CREDENTIAL_ERROR,
    /** A transport or server-side request failure occurred. */
    FIREBASE_AUTH_STATE_NETWORK_ERROR
} firebase_auth_state_t;

/** @brief Email/Password credentials and proactive token-refresh policy. */
typedef struct
{
    /** Firebase Web API key used by Identity Toolkit and Secure Token APIs. */
    const char *api_key;
    /** Firebase Authentication user email. */
    const char *email;
    /** Firebase Authentication user password. */
    const char *password;

    /*
     * Optional UID guard. When non-NULL and non-empty, sign-in succeeds only
     * when Firebase returns this exact UID.
     */
    const char *expected_uid;

    /* Refresh the ID token this many seconds before its real expiration. */
    uint32_t refresh_margin_seconds;
} firebase_auth_config_t;

/** @brief Thread-safe authentication state and request counters. */
typedef struct
{
    /** Current authentication state. */
    firebase_auth_state_t state;

    /** Local ESP-IDF result from the latest authentication operation. */
    esp_err_t last_error;
    /** HTTP status from the latest authentication request. */
    int last_http_status;

    /** Whether an ID token is currently cached. */
    bool has_id_token;
    /** Whether a refresh token is currently cached. */
    bool has_refresh_token;

    /** Number of successful full Email/Password sign-ins. */
    uint32_t successful_sign_in_count;
    /** Number of successful Secure Token refreshes. */
    uint32_t successful_refresh_count;
    /** Number of failed sign-in or refresh requests. */
    uint32_t failed_request_count;

    /** ID token expiration in esp_timer uptime milliseconds. */
    int64_t token_expiry_uptime_ms;

    /**
     * Non-zero generation advanced after token replacement or invalidation.
     *
     * Callers may use this value to prevent reuse of a transport configured
     * with an obsolete authenticated identity.
     */
    uint32_t token_generation;
} firebase_auth_status_t;

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Initialize Firebase Email/Password Authentication.
 *
 * This function only copies configuration and creates internal resources. It
 * does not perform a network request.
 *
 * @param[in] config Credentials, optional UID guard, and refresh margin.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for missing/empty required
 *         values, ESP_ERR_INVALID_SIZE when a copied string is too long,
 *         ESP_ERR_INVALID_STATE if already initialized, or ESP_ERR_NO_MEM if
 *         mutex allocation fails.
 */
esp_err_t firebase_auth_init(
    const firebase_auth_config_t *config);

/**
 * @brief Copy a valid Firebase ID token into the caller buffer.
 *
 * This function may synchronously sign in or refresh the ID token. Call it
 * only from the cloud task or another normal FreeRTOS task, never from an ISR,
 * LVGL callback, Wi-Fi callback, or sensor callback.
 *
 * @param[out] token_buffer Destination for the null-terminated ID token.
 * @param[in] token_buffer_size Destination capacity in bytes; use at least
 *            FIREBASE_AUTH_ID_TOKEN_BUFFER_SIZE.
 * @return ESP_OK with a valid token, an argument/state/size error, a timeout,
 *         or an HTTP/TLS/JSON error from sign-in or refresh.
 */
esp_err_t firebase_auth_get_valid_id_token(
    char *token_buffer,
    size_t token_buffer_size);

/**
 * @brief Invalidate only the cached ID token.
 *
 * The refresh token is preserved so the next request attempts token refresh.
 * This function performs no network operation and uses only the short-lived
 * token-state mutex.
 *
 * @return ESP_OK when invalidated, ESP_ERR_INVALID_STATE before
 *         initialization, or ESP_ERR_TIMEOUT when token state is busy.
 */
esp_err_t firebase_auth_invalidate_id_token(void);

/**
 * @brief Copy the current Firebase Authentication status.
 *
 * @param[out] status Destination for the status snapshot.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if status is NULL,
 *         ESP_ERR_INVALID_STATE before initialization, or ESP_ERR_TIMEOUT if
 *         the authentication mutex cannot be acquired.
 */
esp_err_t firebase_auth_get_status(
    firebase_auth_status_t *status);

#ifdef __cplusplus
}
#endif
