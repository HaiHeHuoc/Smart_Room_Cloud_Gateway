# `firebase_auth` Component

## Purpose

`firebase_auth` implements Firebase Email/Password Authentication for the
ESP32-S3. It signs in through Identity Toolkit, caches the returned ID and
refresh tokens, refreshes the ID token before expiration, optionally validates
the exact user UID, and returns a bounded copy of a currently valid ID token.

The component is synchronous by design. Call it only from a normal
network-owning task such as `cloud_manager`, never from an ISR, Wi-Fi callback,
sensor callback, or LVGL callback.

## Version 1 Status

```text
Release: v1.0.0
Status: Implemented and hardware accepted
```

## Implemented Behavior

- Email/Password sign-in through
  `accounts:signInWithPassword`.
- ID-token refresh through the Google Secure Token endpoint.
- Optional exact UID validation.
- Proactive refresh margin, defaulting to 300 seconds.
- HTTPS certificate verification through `esp_crt_bundle_attach`.
- Full ESP certificate bundle with cross-signed verification enabled.
- Bounded credential, token, URL, request, and response buffers.
- cJSON parsing without intentional credential or token logging.
- URL encoding for refresh-token requests.
- One operation mutex for serialized sign-in/refresh and static HTTP buffers.
- One short-lived state mutex for token/status snapshots and mutations.
- Token generation tracking after replacement or invalidation.
- Zeroization of sensitive temporary buffers.
- Status counters for sign-in, refresh, failure, HTTP result, and token expiry.

## Public API

| API | Role |
|---|---|
| `firebase_auth_init()` | Copy configuration and initialize protected state |
| `firebase_auth_get_valid_id_token()` | Return a valid copied token, signing in or refreshing when required |
| `firebase_auth_invalidate_id_token()` | Clear only the ID token and preserve refresh-token recovery |
| `firebase_auth_get_status()` | Copy state, counters, token flags, generation, and expiry |

Example:

```c
const firebase_auth_config_t config = {
    .api_key = FIREBASE_API_KEY,
    .email = FIREBASE_DEVICE_EMAIL,
    .password = FIREBASE_DEVICE_PASSWORD,
    .expected_uid = FIREBASE_DEVICE_UID,
    .refresh_margin_seconds = 300U,
};

ESP_ERROR_CHECK(firebase_auth_init(&config));

char id_token[FIREBASE_AUTH_ID_TOKEN_BUFFER_SIZE];
ESP_ERROR_CHECK(firebase_auth_get_valid_id_token(
    id_token,
    sizeof(id_token)));
```

Never log `id_token`.

## Token Lifecycle

```text
no refresh token
    -> Email/Password sign-in
    -> cache ID token, refresh token, UID, and expiry

valid ID token
    -> copy token to caller

ID token near expiry
    -> refresh using cached refresh token
    -> atomically replace tokens and expiry

refresh credential rejected
    -> clear refresh token
    -> one full sign-in attempt
```

`firebase_auth_invalidate_id_token()` preserves the refresh token so the next
request attempts refresh first. Invalidation and every successful token
replacement advance a non-zero generation, allowing `cloud_manager` to discard
an HTTP client configured for an obsolete authenticated identity.

## State Model

```text
UNINITIALIZED
    -> INITIALIZED
    -> SIGNING_IN or REFRESHING
    -> AUTHENTICATED
    -> CREDENTIAL_ERROR, NETWORK_ERROR, or INTERNAL_ERROR on failure
```

Token expiry uses `esp_timer` uptime, not wall-clock time; SNTP is not required
for the current expiration calculation.

## Configuration Source

The application maps `FIREBASE_*` macros to project Kconfig values declared in
`main/Kconfig.projbuild`:

```text
Smart Room Cloud Gateway
└── Firebase development configuration
```

The generated `sdkconfig` is ignored by Git. These values are still compiled
into development firmware and must not be described as production secret
storage.

Complete setup, database rules, API-key restrictions, host testing, and public
release controls are documented in:

- [`FIREBASE_SETUP_AND_SECURITY.md`](FIREBASE_SETUP_AND_SECURITY.md)

## Threading And Memory

- Authentication may block for the configured 15-second HTTP timeout.
- Large token/request/response buffers are placed in external RAM where
  configured.
- The operation mutex is held across one sign-in or refresh operation.
- The state mutex is held only for short copies and updates; DNS/TLS/HTTP work
  occurs without the state mutex.
- Token replacement is atomic under the state mutex.
- `get_status()` and invalidation use bounded state-mutex waits.
- The component supports one-shot initialization and has no deinit API.

## Security Contract

- Keep certificate-bundle verification enabled.
- Use a dedicated restricted device account.
- Apply Realtime Database Security Rules based on `auth.uid`.
- Configure the expected UID guard whenever practical.
- Never embed a service-account private key, administrator credential, database
  secret, ID token, or refresh token in source or firmware.
- Never print passwords or tokens.
- Rotate every credential that has entered Git history.
- Do not publish `sdkconfig`, firmware binaries, or logs containing real values.
- Treat a Firebase Web API key as a project identifier, not as database
  authorization; still apply API restrictions and quota controls.
- Production deployments require protected per-device credentials, encrypted
  storage, secure boot/flash encryption, rotation, and revocation.

## Integration Boundary

`firebase_auth` owns authentication and token lifecycle. `cloud_manager` owns
telemetry, network readiness, HTTP client lifecycle, attempt classification,
and retry policy. `main` provides configuration and composition only.

## Troubleshooting

| State / error | Likely checks |
|---|---|
| `CREDENTIAL_ERROR` | Email/password, user enabled, UID guard, response parsing |
| `NETWORK_ERROR` | DNS, Internet, TLS, Firebase availability |
| `INTERNAL_ERROR` | Buffer sizing, allocation, encoding, local request construction |
| HTTP 403 / blocked API | Firebase API-key restrictions and enabled Auth APIs |
| Repeated token rejection | Database rules, expected UID, account state, token generation |

## Related Files

- [`firebase_auth.h`](../include/firebase_auth.h)
- [`firebase_auth.c`](../firebase_auth.c)
- [`cloud_manager` documentation](../../cloud_manager/docs/README.md)
- [`project setup`](../../../../docs/SETUP.md)
- [`security policy`](../../../../SECURITY.md)
