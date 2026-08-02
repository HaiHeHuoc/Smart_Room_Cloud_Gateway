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
- Full default CA bundle with cross-signed certificate verification, required
  for current Google/Firebase alternate certificate chains.
- Bounded request, response, credential, token, and URL buffers.
- JSON parsing through cJSON without logging tokens or credentials.
- URL encoding for the refresh token request body.
- A long-lived operation mutex that serializes synchronous sign-in/refresh and
  owns the static HTTP request/response buffers.
- A separate short-lived state mutex for atomic token/status copies and
  mutations.
- Status snapshots with HTTP result, token generation, and
  sign-in/refresh/failure counters.
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

Initialization copies all configured strings and creates both mutexes. It does
not contact Firebase. The first call to `firebase_auth_get_valid_id_token()`
performs sign-in when no cached token exists.

## Development Configuration Source

The current application maps the four `FIREBASE_*` macros to project Kconfig
symbols declared in `main/Kconfig.projbuild`:

```text
Smart Room Cloud Gateway
└── Firebase development configuration
```

Run `idf.py menuconfig` and set the Web API key, dedicated device account,
password, and optional expected UID. Values are written to the local generated
`sdkconfig`, which is ignored by Git. The repository source intentionally
contains no real Firebase account values.

These values are still compiled into development firmware. Menuconfig prevents
new source commits from carrying them; it is not a production secret-storage
mechanism.

## Public API

| API | Current role |
| --- | --- |
| `firebase_auth_init()` | Copy credentials/policy and initialize protected token state. |
| `firebase_auth_get_valid_id_token()` | Return a copied valid token, signing in or refreshing synchronously when required. |
| `firebase_auth_invalidate_id_token()` | Atomically clear only the cached ID token, preserve the refresh token, advance token generation, and report acceptance. |
| `firebase_auth_get_status()` | Copy state, token-presence flags, token generation, counters, HTTP status, and expiry uptime. |

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

`firebase_auth_invalidate_id_token()` is used after Firebase returns HTTP 401
or for the single forced HTTP 403 recovery. It returns `esp_err_t`, advances a
non-zero token generation, and preserves the refresh token. The next token
request therefore attempts refresh first. A successful sign-in or refresh also
advances the generation, allowing `cloud_manager` to reject an HTTP client
configured for an obsolete authenticated identity.

## State Model

```text
UNINITIALIZED
    -> INITIALIZED
    -> SIGNING_IN or REFRESHING
    -> AUTHENTICATED
    -> CREDENTIAL_ERROR, NETWORK_ERROR, or INTERNAL_ERROR on failure
```

The status expiry value uses `esp_timer` uptime, not wall-clock time. SNTP is
therefore not required for the current expiration calculation.

## Memory And Threading Notes

- Authentication calls may block for the configured 15-second HTTP timeout.
- Static buffers include an 8 KB response buffer, 4 KB request buffer, 4 KB ID
  token buffer, and 2 KB refresh token buffer.
- The operation mutex is held during synchronous sign-in/refresh so only one
  request uses the static URL, request, response, and refresh-snapshot buffers.
- The state mutex is held only while copying or replacing token/status fields.
  It is released before DNS, TLS, and HTTP work, so public status reads and ID
  token invalidation do not wait behind the 15-second network timeout.
- Token replacement is committed atomically under the state mutex; callers
  never copy a partially updated token.
- `get_status()` and token invalidation use bounded state-mutex waits. Token
  retrieval still waits for the active authentication operation because the
  component intentionally supports only one sign-in/refresh at a time.
- There is no deinit API; initialization is one-shot.

## Security Notes

- Keep `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`; do not
  replace bundle validation with a pinned Google intermediate or disabled
  certificate verification.
- A Firebase Web API key identifies the project but is not an administrator
  secret.
- Device account passwords are sensitive and are compiled into development
  firmware from the local menuconfig-generated `sdkconfig`.
- Never embed service-account keys, administrator credentials, private keys,
  ID tokens, or refresh tokens in firmware.
- Authentication response/request buffers and temporary refresh-token copies
  are overwritten after use. Cleanup failures are logged without logging the
  request, response, URL query, credential, or token.
- Restrict Realtime Database access with `auth.uid` rules for the expected
  device UID.
- Use a dedicated restricted account rather than a personal or administrator
  account.
- Rotate every credential that has ever entered Git history. Removing it from
  the current source does not invalidate old commits or clones.
- Move the device credential to protected local configuration, encrypted NVS,
  or a hardware-backed strategy before production deployment.

Firebase project setup and the verified PowerShell flow are documented in
`components/cloud/cloud_manager/README.txt` and `Test/TestFirebase_Auth.ps1`.

## Future Attention

- Add explicit credential rotation and deinit only when required by product
  lifecycle behavior.
- Consider NVS encryption or a hardware-backed credential strategy when the
  board security model is defined.
- Replace application-composition endpoint constants with protected,
  device-specific deployment configuration when production requirements exist.

## Phase 6.4.6 Status

**IMPLEMENTED / HARDWARE TEST PENDING**

The mutex split and observable token invalidation support cloud recovery from
rejected tokens without blocking status access behind network I/O. Hardware
tests must still cover refresh, 401 recovery, persistent 403/credential
failure, cleanup diagnostics, and the full token-expiry window.

## Phase 6.4.7 Closure Status

**IMPLEMENTED / HARDWARE REGRESSION PENDING**

Authentication network operations remain serialized independently from the
short token/status mutex. Sign-in, refresh, rejected-token invalidation, token
generation, and secure temporary-buffer cleanup remain owned here; the cloud
task owns retry policy and HTTP telemetry clients. The final A-N hardware
matrix in the project roadmap covers rejected-token recovery and persistent
authorization failure without a hot authentication loop.

Local URL, JSON, allocation, and refresh-token encoding failures also leave a
terminal diagnostic snapshot instead of retaining `SIGNING_IN` or
`REFRESHING`. They use `INTERNAL_ERROR`, allowing the cloud manager to classify
the underlying `esp_err_t` without treating local failures as rejected
credentials. Partial refresh request/token buffers are cleared before those
errors return.
