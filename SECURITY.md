# Security Policy

## Release Boundary

Version 2.0.0 is portfolio/development firmware, not a production security
certification or fleet deployment baseline. Its public source must contain no
live credentials, private activation/session data, or unsanitized evidence.

## Credential And Provisioning Data

- Wi-Fi credentials are received through BLE provisioning and stored only by
  `config_manager`; never log or publish them.
- Firebase API key, device email/password, and optional UID guard are local
  Menuconfig values compiled into development firmware. Generated `sdkconfig`
  is ignored and must not be committed.
- Firebase ID and refresh tokens, provisioning PoP/QR payloads, service-account
  JSON, private keys, and database URLs containing private identity data are
  secret material.
- Historical exposure is not fixed by deleting current files. Rotate/revoke any
  credential that may have reached Git history before public publication.

## Xiaozhi, WebSocket, And Microphone Privacy

- Xiaozhi activation code/message and managed session credentials are private.
  Do not publish serial output, screenshots, video, or audio that exposes them.
- The firmware uses TLS-backed managed WebSocket services, but that does not
  establish an end-to-end production privacy/security claim for every external
  service or account configuration.
- The microphone is controlled by component-owned capture/PTT policy. Obtain
  consent appropriate to the deployment and do not publish private recordings.
- First-boot activation state is backend-authoritative; do not create a local
  bypass or persist a copied code as an activation credential.

## MCP Controlled Actions

MCP tools expose bounded copied state and invoke public owners. They do not
receive raw Wi-Fi/Firebase/Xiaozhi credentials, driver handles, arbitrary file
paths, or shell control. Controlled action acceptance is not proof of the later
cloud/audio/device result. Destructive device operations such as credential
erase, reboot, reset, and OTA are not exposed as general MCP actions.

## Local Web And SD Storage

- Local Web is a device-network control surface, not an Internet-facing service
  or browser-audio transport. Keep access on a trusted network.
- Validate storage names and operations through the firmware APIs; do not place
  sensitive files on a card intended for public demonstration.
- Logs, diagnostics, downloaded files, screenshots, and media can reveal
  identifiers or operational state. Sanitize before sharing.

## Never Commit Or Publish

- Wi-Fi passwords, Firebase credentials/tokens, or service-account material
- Xiaozhi activation/session data or private transport payloads
- `sdkconfig` with real values, build output, or firmware built with secrets
- Unsanitized serial logs, browser captures, QR/PoP values, or account data
- Audio/video containing private conversations or unrelated personal data

## Owner Release Checklist

- [ ] Rotate/revoke credentials potentially exposed in history.
- [ ] Confirm Firebase rules deny anonymous access and restrict the device path.
- [ ] Review API-key restrictions, quotas, and dedicated device account scope.
- [ ] Verify repository collaborators, branch protections, and CI permissions.
- [ ] Review media and logs against the V2 manifest sanitization rules.
- [ ] Confirm no private configuration, binaries, or evidence artifacts are
      included in the intended public publication.

## Production Hardening Not Included

The release does not claim secure boot, flash/NVS encryption, signed OTA with
rollback, hardware-backed keys, fleet identity/revocation, rate limiting, App
Check, or continuous security monitoring. A production deployment requires a
separate threat model, manufacturing/provisioning process, and verification.

## Reporting A Vulnerability

Do not open a public issue containing credentials, tokens, private URLs, or
exploit details. Use GitHub private vulnerability reporting/Security Advisory
when available, or contact the repository owner privately with a sanitized
reproduction, affected commit, impact, and suggested mitigation.
