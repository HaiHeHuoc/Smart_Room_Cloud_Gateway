# Security Policy

## Supported Release

| Release | Status |
|---|---|
| `v1.0.0` | Supported as a portfolio/development release |
| Version 2 development branches | Best effort until formally released |

## Repository Access Model

This repository is public so people can inspect, download, clone, and fork the
source code. Public visibility does not grant write access to the original
repository.

The intended access policy is:

- unauthenticated and external users: read, clone, download, and fork only;
- external contributors: propose changes through Pull Requests from forks;
- explicitly authorized collaborators: only the minimum required permission;
- repository owner: final approval, merge, release, settings, and recovery;
- `main`: protected by a GitHub branch ruleset.

No external user can push, delete branches, merge Pull Requests, modify
settings, or publish releases unless the repository owner explicitly grants a
GitHub permission that allows it.

Repository files support this policy:

- `.github/CODEOWNERS` assigns all paths to `@HaiHeHuoc`;
- `CONTRIBUTING.md` requires fork/branch/Pull Request contribution flow;
- `.github/PULL_REQUEST_TEMPLATE.md` requires validation and security review;
- `.github/workflows/repository-policy.yml` runs read-only policy checks.

These files do not replace GitHub permissions or branch protection. The owner
must keep the `main` ruleset active and avoid granting unnecessary Write,
Maintain, or Admin access.

## Required `main` Branch Ruleset

The active GitHub ruleset for `main` should enforce:

- require a Pull Request before merging;
- require review from Code Owners;
- require the `Repository policy` status check;
- require review conversations to be resolved;
- block force pushes;
- restrict branch deletion;
- prevent non-fast-forward updates;
- apply rules to administrators unless an emergency bypass is deliberately
  retained for the repository owner only.

A ruleset is the enforcement boundary. `CODEOWNERS` requests ownership review,
but GitHub enforces it only when the corresponding branch rule is enabled.

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

Before publishing a new public release:

- [ ] Rotate every password or token that has ever appeared in Git history.
- [ ] Confirm superseded Firebase device-account passwords no longer work.
- [ ] Inspect or rewrite Git history so no still-sensitive value remains.
- [ ] Confirm current source and tests contain no real password, email, ID token,
      refresh token, service-account key, or private key.
- [ ] Confirm Realtime Database rules deny anonymous reads and writes.
- [ ] Restrict the dedicated device UID to its intended database path.
- [ ] Review Firebase API-key restrictions and Identity Toolkit quotas.
- [ ] Confirm `sdkconfig`, build outputs, logs, and firmware images are ignored.
- [ ] Review all media for SSIDs, QR payloads, account data, tokens, and private
      browser information.
- [ ] Review repository collaborators and remove unnecessary write access.
- [ ] Confirm the `main` ruleset and required checks are active.
- [ ] Decide whether to add an explicit repository license.

A documentation or source cleanup commit does not invalidate a secret already
stored in Git history. Rotation/revocation is mandatory for previously exposed
passwords and tokens.

## Workflow Security

GitHub Actions workflows must:

- use the minimum required permissions;
- default to `contents: read` unless a documented write operation is required;
- avoid `pull_request_target` for untrusted fork code;
- avoid exposing repository secrets to forked Pull Requests;
- pin and review third-party actions before adoption;
- keep checkout credentials disabled when no push is required;
- never automatically merge or publish from untrusted contributions.

The repository-policy workflow is intentionally read-only and uses
`persist-credentials: false`.

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
