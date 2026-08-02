# Sprint 9 — Portfolio Polish Status

**Date:** 2026-08-02  
**Repository branch:** `main`  
**Status:** Implemented / Real Media Evidence Pending

## Goal

Make the ESP32-S3 Smart Room Cloud Gateway understandable, buildable, demonstrable, and useful for GitHub, CV, and technical interviews.

## Completed Work

- [x] Added a root `README.md` with project overview, features, hardware, pin map, architecture, setup, runtime flows, resource snapshot, security notes, limitations, and interview talking points.
- [x] Added a component ownership and runtime architecture document.
- [x] Added Mermaid architecture and state-machine diagrams.
- [x] Added a hardware wiring, Firebase configuration, build, flash, and troubleshooting guide.
- [x] Added a deterministic portfolio demo script and evidence checklist.
- [x] Added a media naming and sanitization guide.
- [x] Added known limitations, deliberately deferred features, and future improvements.
- [x] Removed real Firebase account values from the current source head.
- [x] Added project menuconfig entries for local Firebase development configuration.
- [x] Updated firmware release date metadata for the portfolio revision.

## Pending User-Supplied Evidence

The following tasks require the physical prototype and cannot be fabricated from repository source:

- [ ] Commit real hardware photos.
- [ ] Commit sanitized LCD/Firebase/performance screenshots.
- [ ] Record and upload the short hardware demo video.
- [ ] Add final media links to the root README.

Use:

- `docs/DEMO.md`
- `docs/media/README.md`

## Security Follow-Up Required

Removing credentials from the current source does not remove them from Git history.

Before broad public publication:

1. Rotate the dedicated Firebase device account credential.
2. Review database rules and account permissions.
3. Consider rewriting repository history after rotation.
4. Verify no provisioning secret, token, QR payload, SSID/password, or private URL appears in screenshots, logs, or video.

## Definition Of Done Assessment

| Criterion | Result |
|---|---|
| Another developer can understand the project from README | Complete |
| Build and setup steps are clear | Complete |
| Architecture is explainable in an interview | Complete |
| Project has a clear demo path | Complete |
| Real photo/screenshot evidence exists | Pending |
| Demo video exists | Pending |

## Files Added Or Updated

```text
README.md
PHASE_9_PORTFOLIO_STATUS.md
main/Kconfig.projbuild
components/system/common/include/app_common.h
docs/ARCHITECTURE.md
docs/SETUP.md
docs/DEMO.md
docs/KNOWN_LIMITATIONS.md
docs/media/README.md
```

## Closure Rule

Sprint 9 should be marked fully `Done` only after real media evidence and the final demo link are added. Until then, the accurate status is:

```text
IMPLEMENTED / MEDIA EVIDENCE PENDING
```
