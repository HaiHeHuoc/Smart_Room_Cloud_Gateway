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
#define FIREBASE_AUTH_ID_TOKEN_BUFFER_SIZE    4096U

/* Type Definitions --------------------------------------------------------- */
typedef enum
{
    FIREBASE_AUTH_STATE_UNINITIALIZED = 0,
    FIREBASE_AUTH_STATE_INITIALIZED,
    FIREBASE_AUTH_STATE_SIGNING_IN,
    FIREBASE_AUTH_STATE_REFRESHING,
    FIREBASE_AUTH_STATE_AUTHENTICATED,
    FIREBASE_AUTH_STATE_CREDENTIAL_ERROR,
    FIREBASE_AUTH_STATE_NETWORK_ERROR
} firebase_auth_state_t;

typedef struct
{
    const char *api_key;
    const char *email;
    const char *password;

    /*
     * Optional UID guard. When non-NULL and non-empty, sign-in succeeds only
     * when Firebase returns this exact UID.
     */
    const char *expected_uid;

    /* Refresh the ID token this many seconds before its real expiration. */
    uint32_t refresh_margin_seconds;
} firebase_auth_config_t;

typedef struct
{
    firebase_auth_state_t state;

    esp_err_t last_error;
    int last_http_status;

    bool has_id_token;
    bool has_refresh_token;

    uint32_t successful_sign_in_count;
    uint32_t successful_refresh_count;
    uint32_t failed_request_count;

    int64_t token_expiry_uptime_ms;
} firebase_auth_status_t;

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Initialize Firebase Email/Password Authentication.
 *
 * This function only copies configuration and creates internal resources. It
 * does not perform a network request.
 */
esp_err_t firebase_auth_init(
    const firebase_auth_config_t *config);

/**
 * @brief Copy a valid Firebase ID token into the caller buffer.
 *
 * This function may synchronously sign in or refresh the ID token. Call it
 * only from the cloud task or another normal FreeRTOS task, never from an ISR,
 * LVGL callback, Wi-Fi callback, or sensor callback.
 */
esp_err_t firebase_auth_get_valid_id_token(
    char *token_buffer,
    size_t token_buffer_size);

/**
 * @brief Invalidate only the cached ID token.
 *
 * The refresh token is preserved so the next request attempts token refresh.
 */
void firebase_auth_invalidate_id_token(void);

/**
 * @brief Copy the current Firebase Authentication status.
 */
esp_err_t firebase_auth_get_status(
    firebase_auth_status_t *status);

#ifdef __cplusplus
}
#endif
