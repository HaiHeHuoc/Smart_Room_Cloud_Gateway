# Firebase Authentication And Security Setup

This guide configures the Firebase services used by the ESP32-S3 Smart Room
Cloud Gateway and documents the minimum security controls required before the
repository is published.

The current firmware uses:

- Firebase Authentication with Email/Password;
- the Identity Toolkit REST sign-in endpoint;
- the Secure Token REST refresh endpoint;
- Firebase ID tokens for authenticated Realtime Database REST requests;
- an optional exact UID guard in `firebase_auth`;
- Realtime Database Security Rules as the authorization boundary.

This is a client-device authentication design for a portfolio/development
release. The device password is compiled into the local firmware image and is
not a production-grade secret-storage design.

## 1. Create Or Select A Firebase Project

1. Open Firebase Console.
2. Create a project or select an existing project dedicated to this gateway.
3. Open **Project settings > General**.
4. Add a Web app if the project does not already expose a Web API key.
5. Record locally, but do not commit:
   - the Firebase Web API key;
   - the Realtime Database URL;
   - the dedicated device account email;
   - the dedicated device account password;
   - the device account UID.

A Firebase Web API key identifies the Firebase project; it is not the database
authorization mechanism. Data access must be protected by Authentication and
Realtime Database Security Rules.

## 2. Enable Email/Password Authentication

1. Open **Build > Authentication**.
2. Open **Sign-in method**.
3. Enable **Email/Password**.
4. Save the provider configuration.
5. Open **Users** and create one dedicated device user.

Use a dedicated account such as a device-specific mailbox. Do not use a
personal Google account, Firebase owner account, administrator account, or a
service-account private key in firmware.

After creating the user, copy its UID. The firmware can compare the UID returned
by Firebase with `CONFIG_APP_FIREBASE_DEVICE_UID` so credentials for an
unexpected account are rejected.

## 3. Create Realtime Database

1. Open **Build > Realtime Database**.
2. Create the database in the region appropriate for the deployment.
3. Copy the exact database URL.
4. Keep the database closed by default; do not leave test-mode public rules.

The Version 1 application writes its latest telemetry object under:

```text
/devices/esp32s3-001/latest
```

The endpoint in `main/main.c` must end in `.json` and must not already contain
query parameters. `cloud_manager` adds the Firebase ID token and
`print=silent` internally.

## 4. Apply Realtime Database Security Rules

Replace `<DEVICE_UID>` with the UID of the dedicated Firebase Authentication
user. The following baseline denies all unspecified access and allows only that
identity to read or write the current device path:

```json
{
  "rules": {
    ".read": false,
    ".write": false,
    "devices": {
      "esp32s3-001": {
        ".read": "auth != null && auth.uid === '<DEVICE_UID>'",
        ".write": "auth != null && auth.uid === '<DEVICE_UID>'",
        "latest": {
          ".validate": "newData.hasChildren(['temperature_c', 'humidity_percent', 'sensor_valid', 'sensor_stale', 'sensor_state', 'last_error', 'sample_uptime_ms', 'source'])",
          "temperature_c": {
            ".validate": "newData.isNumber() && newData.val() >= -40 && newData.val() <= 85"
          },
          "humidity_percent": {
            ".validate": "newData.isNumber() && newData.val() >= -1 && newData.val() <= 100"
          },
          "sensor_valid": {
            ".validate": "newData.isBoolean()"
          },
          "sensor_stale": {
            ".validate": "newData.isBoolean()"
          },
          "sensor_state": {
            ".validate": "newData.isNumber()"
          },
          "last_error": {
            ".validate": "newData.isNumber()"
          },
          "sample_uptime_ms": {
            ".validate": "newData.isNumber() && newData.val() >= 0"
          },
          "source": {
            ".validate": "newData.isString() && newData.val() === 'esp32_cloud_manager'"
          }
        }
      }
    }
  }
}
```

The `-1` lower bound accommodates the current finite invalid-reading sentinel.
Change the rules together with the telemetry schema if that representation is
changed later.

Publish the rules, then verify:

- an unauthenticated GET/PUT is rejected;
- the dedicated device user can access only its intended path;
- another authenticated user cannot read or write the device path;
- malformed telemetry is rejected by validation rules.

## 5. Review API-Key Restrictions And Quotas

In Google Cloud Console, open **APIs & Services > Credentials** and inspect the
Firebase Web API key.

- Keep API restrictions limited to the Firebase-related APIs required by this
  project.
- Do not reuse this key for unrelated or billable Google APIs.
- Never add the Generative Language API to this client key.
- If `API_KEY_SERVICE_BLOCKED` appears, verify that the required Firebase
  Authentication APIs are present in the allowlist.
- For password-based Authentication, set Identity Toolkit quotas to a level
  consistent with expected device traffic to reduce brute-force abuse.

Do not treat obscuring the API key as authorization. Security Rules protect the
database; the dedicated user's password, ID token, and refresh token remain
sensitive credentials.

## 6. Configure The Firmware Locally

From the project root:

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

Open:

```text
Smart Room Cloud Gateway
└── Firebase development configuration
```

Set:

| Menuconfig field | Purpose |
|---|---|
| Firebase Web API key | Identifies the Firebase project for Auth REST calls |
| Firebase device account email | Dedicated Email/Password user |
| Firebase device account password | Sensitive development credential |
| Expected Firebase device UID | Optional exact identity guard; recommended |

The values are written into the generated local `sdkconfig`, which is ignored
by Git. They are still compiled into the firmware image.

Never commit:

```text
sdkconfig
sdkconfig.old
Firebase passwords
ID tokens
refresh tokens
service-account JSON files
private keys
firmware binaries containing real credentials
```

## 7. Configure The Database Endpoint

Version 1 currently defines the endpoint in the application composition root:

```c
static const cloud_manager_config_t CLOUD_MANAGER_CONFIG =
{
    .firebase_latest_url =
        "https://<database-name>.<region>.firebasedatabase.app/"
        "devices/esp32s3-001/latest.json",
    .publish_period_ms = 10000U,
};
```

Use the exact database host shown by Firebase Console. Do not append
`?auth=...`; `cloud_manager` obtains a valid ID token from `firebase_auth` and
adds authentication internally.

The database hostname and project ID are identifiers, not administrator
secrets. They should nevertheless be kept configurable in a multi-environment
or production deployment.

## 8. Run The Host Authentication Test

The sanitized PowerShell test reads configuration from environment variables:

```powershell
$env:FIREBASE_API_KEY = "<web-api-key>"
$env:FIREBASE_DEVICE_EMAIL = "<device-email>"
$env:FIREBASE_DATABASE_URL = "https://<database-name>.<region>.firebasedatabase.app"
$env:FIREBASE_DEVICE_ID = "esp32s3-001"
$env:FIREBASE_DEVICE_UID = "<expected-uid>"

# Prefer the secure prompt instead of putting the password in shell history.
Remove-Item Env:FIREBASE_DEVICE_PASSWORD -ErrorAction SilentlyContinue
.\Test\TestFirebase_Auth.ps1
```

The script prompts securely for the password when
`FIREBASE_DEVICE_PASSWORD` is absent. It does not print ID or refresh tokens.

Expected high-level result:

```text
Firebase login successful
Authenticated Firebase PUT successful
```

## 9. Build, Flash, And Verify

```bash
idf.py fullclean
idf.py build
idf.py -p <PORT> flash monitor
```

Verify:

1. `firebase_auth_init()` succeeds.
2. The first cloud upload performs Email/Password sign-in.
3. Firebase returns the configured UID.
4. The UID guard accepts only the expected account.
5. Realtime Database receives the latest telemetry object.
6. A later upload reuses or refreshes the token without logging token data.
7. Wi-Fi loss enters Cloud Wait/Retry and recovery returns to Cloud Online.
8. Invalid credentials enter a bounded authentication error rather than a hot
   retry loop.

## 10. Runtime Security Properties

The component currently provides:

- HTTPS verification through the ESP certificate bundle;
- bounded credential, token, URL, request, and response buffers;
- serialized sign-in/refresh operations;
- short-lived protected state snapshots;
- optional exact UID validation;
- ID-token refresh before expiration;
- token generation tracking after replacement or invalidation;
- zeroization of sensitive temporary buffers;
- no intentional credential or token logging.

Do not call authentication APIs from an ISR, LVGL callback, Wi-Fi callback, or
sensor callback. Network operations can block for the configured HTTP timeout
and belong to the cloud task.

## 11. Public-Repository And Production Boundaries

Before making the repository public:

- rotate every password or token that has ever appeared in Git history;
- remove real credentials from the current tree and test scripts;
- rewrite repository history if it contains a still-sensitive credential;
- verify database rules deny anonymous access;
- restrict the Firebase API key and review Identity Toolkit quotas;
- sanitize screenshots, QR codes, logs, and videos;
- do not publish firmware binaries built with real credentials.

For a production device, also define and validate:

- per-device credentials instead of one shared password;
- protected provisioning/manufacturing data;
- NVS encryption, flash encryption, and secure boot;
- credential rotation and revocation;
- secure OTA and rollback policy;
- an abuse-control strategy such as App Check or a project-owned backend.

App Check is not implemented by this ESP-IDF client in Version 1. Do not claim
App Check protection until the firmware can obtain and send a valid App Check
token and the backend enforces it.

## 12. Troubleshooting

| Symptom | Checks |
|---|---|
| `EMAIL_NOT_FOUND` / invalid credential | Dedicated user exists and email/password are correct |
| `USER_DISABLED` | Re-enable or replace the device user |
| UID mismatch | Set the exact Authentication UID in menuconfig |
| HTTP 403 / `API_KEY_SERVICE_BLOCKED` | Review API restrictions and enabled Firebase Auth APIs |
| Database `Permission denied` | Publish rules and verify `auth.uid` and path |
| Unauthenticated script unexpectedly succeeds | Database rules are too permissive; close them immediately |
| TLS verification failure | Keep the ESP certificate bundle and cross-signed verification enabled |
| Repeated sign-in | Check refresh-token handling, clock-independent expiry uptime, and credential rejection logs |

## References Inside This Repository

- [`README.md`](README.md) — component design and runtime behavior
- [`firebase_auth.h`](../include/firebase_auth.h) — public API contract
- [`cloud_manager` notes](../../cloud_manager/docs/README.md) — authenticated telemetry owner
- [`project setup`](../../../../docs/SETUP.md) — complete build and hardware setup
- [`TestFirebase_Auth.ps1`](../../../../Test/TestFirebase_Auth.ps1) — sanitized host test
