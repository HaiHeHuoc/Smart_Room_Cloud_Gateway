# Contributing

Thank you for your interest in the ESP32-S3 Smart Room Cloud Gateway.

This repository is public so people can inspect, clone, study, and evaluate the
source code. Public visibility does not grant write access to the original
repository.

## Access Model

- Everyone may view, clone, and fork the public repository.
- Only explicitly authorized maintainers may push to this repository.
- External contributors must work from a fork or a separate branch and submit a
  Pull Request.
- Opening a Pull Request does not grant permission to merge or modify the
  protected branch.
- The repository owner decides whether a proposal is accepted, revised, or
  closed.

## Required Workflow

```text
fork repository
    -> create a focused branch
    -> make and test one logical change
    -> remove private data and generated artifacts
    -> open a Pull Request against main
    -> owner review
    -> merge only after approval and required checks
```

Do not ask for direct push access for a normal contribution.

## Branch Rules

The intended policy for `main` is:

- Pull Requests are required before merge.
- Direct pushes are blocked for non-bypass users.
- Force pushes are blocked.
- Branch deletion is blocked.
- Code-owner review is required.
- Required checks must pass.
- Review conversations must be resolved.

These rules are enforced through GitHub repository settings, not by this file
alone.

## Change Scope

Keep each Pull Request focused. Avoid unrelated formatting, renaming, component
reorganization, or architecture changes.

For embedded changes, include:

- hardware assumptions;
- ESP-IDF version and target board;
- build result;
- flash/hardware result when applicable;
- memory, stack, timing, ISR, and concurrency impact;
- logs with credentials and personal information removed.

## Security And Sensitive Data

Never commit or attach:

- Firebase passwords, ID tokens, or refresh tokens;
- service-account JSON files, administrator credentials, or private keys;
- Wi-Fi credentials;
- provisioning Proof of Possession values or readable QR payloads;
- local `sdkconfig` files containing real configuration;
- `.env` or credential files;
- firmware binaries built with real credentials;
- NVS dumps, serial logs, screenshots, or videos containing private data.

A secret must be rotated or revoked if it is accidentally committed. Deleting
it in a later commit is not enough because Git history may retain it.

For vulnerability reports, follow [`SECURITY.md`](SECURITY.md) instead of
opening a public issue with exploit details or credentials.

## Build Validation

Use the project baseline unless the Pull Request explicitly changes it:

```bash
idf.py set-target esp32s3
idf.py build
```

Hardware-affecting changes should also record:

```text
Board:
ESP-IDF version:
Commit:
Wiring changes:
Build result:
Flash result:
Hardware test result:
Known limitations:
```

## Pull Request Checklist

Before submitting:

- [ ] The change has one clear purpose.
- [ ] No credential, token, private key, local config, or private media is included.
- [ ] Generated build artifacts are not committed.
- [ ] Documentation matches the implemented behavior.
- [ ] `idf.py build` passes, or the reason it was not run is stated.
- [ ] Hardware validation is included when the change affects runtime behavior.
- [ ] Existing ownership boundaries and LVGL/threading rules are preserved.
- [ ] The Pull Request explains risks, rollback, and remaining limitations.

## Licensing And Reuse

The repository currently has no explicit open-source license. Public visibility
allows inspection and cloning through GitHub, but it does not automatically
grant broad rights to redistribute or reuse the source. See the root README for
the current licensing statement.
