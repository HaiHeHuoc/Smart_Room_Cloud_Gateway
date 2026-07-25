# firebase_auth Component Notes

## Purpose

`firebase_auth` implements Firebase Email/Password Authentication for the
ESP32-S3. It signs in through Identity Toolkit, caches the returned ID and
refresh tokens, refreshes the ID token before expiration, and gives callers a
bounded copy of a currently valid ID token.

The component is synchronous by design. Call it from a normal network-owning
task such as `cloud_manager`, never from an ISR, event callback, sensor
callback, or LVGL callback.

## What Is Implemented

- Email/Password sign-in through `accounts:signInWithPassword`.
- ID-token refresh through the Google Secure Token endpoint.
- Optional exact UID validation after sign-in.
- Proactive refresh margin, defaulting to 300 seconds.
- HTTPS certificate verification through `esp_crt_bundle_attach`.
- Bounded request, response, credential, token, and URL buffers.
- JSON parsing through cJSON without logging tokens or credentials.
- URL encoding for the refresh token request body.
- A mutex serializing token state and synchronous authentication requests.
- Status snapshots with HTTP result and sign-in/refresh/failure counters.
- Refresh failure fallback to full sign-in only for credential errors, not for
  ordinary transport failures.

## Initialization

```c
const firebase_auth_config_t auth_config = {
    .api_key = FIREBASE_API_KEY,
    .email = FIREBASE_DEVICE_EMAIL,
    .password = FIREBASE_DEVICE_PASSWORD,
    .expected_uid = FIREBASE_DEVICE_UID,
    .refresh_margin_seconds = 300U,
};

ESP_ERROR_CHECK(firebase_auth_init(&auth_config));
```

Initialization copies all configured strings and creates the mutex. It does
not contact Firebase. The first call to `firebase_auth_get_valid_id_token()`
performs sign-in when no cached token exists.

## Public API

| API | Current role |
| --- | --- |
| `firebase_auth_init()` | Copy credentials/policy and initialize protected token state. |
| `firebase_auth_get_valid_id_token()` | Return a copied valid token, signing in or refreshing synchronously when required. |
| `firebase_auth_invalidate_id_token()` | Clear only the cached ID token while preserving the refresh token. |
| `firebase_auth_get_status()` | Copy state, token-presence flags, counters, HTTP status, and expiry uptime. |

Use a destination sized by the public macro:

```c
char id_token[FIREBASE_AUTH_ID_TOKEN_BUFFER_SIZE];

esp_err_t ret = firebase_auth_get_valid_id_token(
    id_token,
    sizeof(id_token));
```

Do not log the returned buffer.

## Token Lifecycle

```text
no refresh token
    -> Email/Password sign-in
    -> cache ID token + refresh token + UID + expiry

valid ID token
    -> return copied token

ID token near expiry
    -> refresh with cached refresh token
    -> cache replacement tokens and expiry

refresh credential rejected
    -> clear refresh token
    -> one full Email/Password sign-in
```

`firebase_auth_invalidate_id_token()` is used after Firebase returns HTTP 401.
The next token request first attempts refresh because the refresh token remains
cached.

## State Model

```text
UNINITIALIZED
    -> INITIALIZED
    -> SIGNING_IN or REFRESHING
    -> AUTHENTICATED
    -> CREDENTIAL_ERROR or NETWORK_ERROR on failure
```

The status expiry value uses `esp_timer` uptime, not wall-clock time. SNTP is
therefore not required for the current expiration calculation.

## Memory And Threading Notes

- Authentication calls may block for the configured 15-second HTTP timeout.
- Static buffers include an 8 KB response buffer, 4 KB request buffer, 4 KB ID
  token buffer, and 2 KB refresh token buffer.
- The authentication mutex is held during synchronous sign-in/refresh so only
  one caller can mutate token state.
- `get_status()` and token invalidation use bounded mutex waits; token retrieval
  waits until the active authentication operation completes.
- There is no deinit API; initialization is one-shot.

## Security Notes

- A Firebase Web API key identifies the project but is not an administrator
  secret.
- Device passwords are sensitive and are currently compiled into the firmware
  through development configuration.
- Never embed service-account keys, administrator credentials, private keys,
  ID tokens, or refresh tokens in firmware.
- Restrict Realtime Database access with `auth.uid` rules for the expected
  device UID.
- Move the device password to provisioning or protected local configuration
  before production deployment.

Firebase project setup and the verified PowerShell flow are documented in
`components/cloud/cloud_manager/README.txt` and `Test/TestFirebase_Auth.ps1`.

## Future Attention

- Provision credentials without committing them to shared headers.
- Consider NVS encryption or a hardware-backed credential strategy when the
  board security model is defined.
- Add explicit credential rotation and deinit only when required by product
  lifecycle behavior.
