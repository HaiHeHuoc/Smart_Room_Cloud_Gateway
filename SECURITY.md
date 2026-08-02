# Security Policy

## Supported Release

| Release | Status |
|---|---|
| `v1.0.0` | Supported as a portfolio/development release |
| Version 2 development branches | Best effort until formally released |

## Security Model

Version 1 is an embedded portfolio release, not a production security baseline.
It uses:

- Firebase Email/Password Authentication with a dedicated device account;
- Firebase ID tokens for authenticated Realtime Database REST requests;
- Realtime Database Security Rules for authorization;
- TLS certificate verification through the ESP-IDF certificate bundle;
- local `menuconfig` values stored in the generated, Git-ignored `sdkconfig`;
- bounded buffers and deliberate zeroization of sensitive temporary data.

The Firebase device password is still compiled into the development firmware.
Anyone with physical access and sufficient capability may extract firmware or
inspect runtime memory. Version 1 does not claim hardware-backed secret storage.

## Never Commit Or Publish

- Firebase device passwords;
- ID tokens or refresh tokens;
- service-account JSON files, administrator credentials, or private keys;
- Wi-Fi passwords;
- provisioning Proof of Possession values or readable QR payloads;
- local `sdkconfig` files containing real configuration;
- firmware binaries built with real credentials;
- unredacted serial logs, screenshots, or videos containing private data.

## Public-Release Checklist

Before changing the repository visibility to public:

- [ ] Rotate every password or token that has ever appeared in Git history.
- [ ] Confirm the old Firebase device-account password no longer works.
- [ ] Inspect or rewrite Git history so no still-sensitive value remains.
- [ ] Confirm current source and tests contain no real password, email, ID token,
      refresh token, service-account key, or private key.
- [ ] Confirm Realtime Database rules deny anonymous reads and writes.
- [ ] Restrict the dedicated device UID to its intended database path.
- [ ] Review Firebase API-key restrictions and Identity Toolkit quotas.
- [ ] Confirm `sdkconfig`, build outputs, logs, and firmware images are ignored.
- [ ] Review all media for SSIDs, QR payloads, account data, tokens, and private
      browser information.
- [ ] Decide whether to add an explicit repository license.

A documentation or source cleanup commit does not invalidate a secret already
stored in Git history. Rotation/revocation is mandatory for previously exposed
passwords and tokens.

## Reporting A Vulnerability

Do not open a public issue containing credentials, tokens, private URLs, or
exploit details.

Prefer GitHub private vulnerability reporting or a private Security Advisory
when available. Otherwise, contact the repository owner privately and provide:

- affected release and commit;
- reproduction steps;
- impact and affected data;
- logs with all credentials and personal information removed;
- a suggested mitigation when available.

## Production Hardening Not Included In Version 1

A product deployment should additionally define and verify:

- unique per-device identity and credential rotation;
- protected manufacturing/provisioning data;
- NVS encryption, flash encryption, and secure boot;
- signed OTA with rollback policy;
- device revocation and recovery;
- rate limiting and abuse monitoring;
- App Check or a project-owned authenticated backend where appropriate;
- automated secret scanning and dependency/security checks.

See
[`components/cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md`](components/cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md)
for Firebase-specific setup and controls.
