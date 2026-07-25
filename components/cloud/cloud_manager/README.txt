# Firebase Connection Setup

This file records the Firebase project information and the verified
Email/Password Authentication flow used by this project.

Reference test script:

```text
Test/TestFirebase_Auth.ps1
```

## Firebase project

```text
Project ID:          esp32-smart-room-gateway
Project number:      89123245513
Auth domain:         esp32-smart-room-gateway.firebaseapp.com
Realtime Database:   https://esp32-smart-room-gateway-default-rtdb.asia-southeast1.firebasedatabase.app
Storage bucket:      esp32-smart-room-gateway.firebasestorage.app
Web API key:         AIzaSyBXsyDzNYGd0xxRDvms8nnwtuIYwR3h8ks
Web app ID:          1:89123245513:web:ebb1c859ee2417306b2c18
Measurement ID:      G-JCE7P6L63C
```

The Firebase Web API key identifies the Firebase project. It is not an
administrator credential, but its use should still be restricted in the
Google Cloud/Firebase project where practical.

## Device identity

```text
Device ID:           esp32s3-001
Authentication UID: 0leuYu7fCMRnM8w5bvv8VzquPgV2
Authentication type: Email/Password
```

The authentication email is configured locally in
`Test/TestFirebase_Auth.ps1`. Do not store the password, ID token, or refresh
token in this README or commit them to source control.

## Firebase Console setup

1. Open Firebase Console and select `esp32-smart-room-gateway`.
2. Under Authentication, enable the Email/Password sign-in provider.
3. Under Authentication > Users, create the device user used by the test
   script and confirm that its UID matches the UID above.
4. Create or select the Realtime Database in the `asia-southeast1` region.
5. Configure Realtime Database Security Rules so this UID can access only its
   intended device path.

Example rules for the current device path:

```json
{
  "rules": {
    "devices": {
      "esp32s3-001": {
        ".read": "auth != null && auth.uid === '0leuYu7fCMRnM8w5bvv8VzquPgV2'",
        ".write": "auth != null && auth.uid === '0leuYu7fCMRnM8w5bvv8VzquPgV2'"
      }
    }
  }
}
```

Review these rules before production use. Add validation rules for telemetry
fields when the final database schema is stable.

## Authentication REST request

The test script signs in through Firebase Identity Toolkit:

```text
POST https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=<WEB_API_KEY>
```

Request body:

```json
{
  "email": "<DEVICE_EMAIL>",
  "password": "<DEVICE_PASSWORD>",
  "returnSecureToken": true
}
```

A successful response provides:

- `localId`: the authenticated UID.
- `idToken`: short-lived token used for Realtime Database requests.
- `refreshToken`: token used to obtain a new ID token.
- `expiresIn`: ID token lifetime in seconds; currently reported as 3600.

Never print or commit the password, ID token, or refresh token.

## Authenticated Realtime Database request

Current telemetry endpoint:

```text
https://esp32-smart-room-gateway-default-rtdb.asia-southeast1.firebasedatabase.app/devices/esp32s3-001/latest.json
```

The PowerShell test appends the ID token and suppresses the successful PUT
response body:

```text
?auth=<ID_TOKEN>&print=silent
```

The verified test sequence is:

1. Sign in with Email/Password.
2. Keep `idToken` and `refreshToken` private.
3. Send an HTTP PUT to the telemetry endpoint with `auth=<ID_TOKEN>`.
4. Treat login or database HTTP errors as test failures.

Run the reference test from the project root:

```powershell
.\Test\TestFirebase_Auth.ps1
```

Expected output:

```text
Firebase login successful
UID:        0leuYu7fCMRnM8w5bvv8VzquPgV2
Expires in: 3600 seconds
Authenticated Firebase PUT successful
```

## Firmware integration status

The firmware now implements the verified flow through two components:

- `firebase_auth` performs Email/Password sign-in, validates the configured
  UID, caches tokens, and refreshes the ID token before expiration.
- `cloud_manager` obtains a valid ID token and sends telemetry to
  `latest.json?auth=<ID_TOKEN>&print=silent`.

`cloud_manager` reports HTTP 401/403 as authentication errors, invalidates an
ID token after HTTP 401, retains pending telemetry across failures, and retries
transport/retryable HTTP failures with bounded backoff.

Component implementation notes are in:

```text
components/cloud/firebase_auth/docs/README.md
components/cloud/cloud_manager/docs/README.md
```

The device email/password are still development configuration compiled into
the firmware. Move them to provisioning or protected local configuration
before production use. Do not print or commit passwords, ID tokens, or refresh
tokens.

Do not store Firebase administrator secrets, service-account private keys, or
other privileged server credentials in ESP32 firmware.
