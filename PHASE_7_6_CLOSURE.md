# Phase 7.6 — Final Hardware Regression and Sprint 7 Closure

## Status

**COMPLETE / HARDWARE ACCEPTANCE CONFIRMED BY USER**

- Acceptance date: 2026-08-02
- Target: ESP32-S3 N16R8
- Framework: ESP-IDF v6.0.1
- Change type for this closure: documentation, comments, and docstrings only
- Production behavior changed by this closure: none

Phase 7.6 closes Sprint 7 after the user confirmed completion of the Phase 7.4
reset-result UI and Phase 7.5 active-network reset coordination. This closure
records the accepted behavior, documentation state, resource baseline, and
intentionally deferred work. It does not introduce a new feature or alter the
factory-reset implementation.

## Closed Scope

The accepted Sprint 7 path is:

```text
short press
    -> no reset

long press
    -> one qualified reset request
    -> quiesce provisioning and Station ownership
    -> suppress reconnect and late persistence/adoption
    -> clear ESP-IDF driver-owned Wi-Fi persistence
    -> clear and verify application Wi-Fi configuration
    -> present reset result through the GUI task
    -> controlled reboot
    -> boot as NOT_CONFIGURED
    -> start BLE provisioning
    -> accept new credentials
    -> obtain IPv4 without erase-flash
```

The final ownership boundaries remain:

- `button_manager` owns GPIO polling, debounce, and long-press publication.
- `app_reset_coordinator` owns press-cycle qualification and the reset
  transaction.
- `app_network_coordinator` owns reset exclusion, provisioning quiescence, and
  Station detach coordination.
- `wifi_manager` owns Station connect, disconnect, reconnect, and driver-owned
  persistence cleanup.
- `config_manager` owns application Wi-Fi configuration erasure and
  verification.
- `provisioning_manager` owns temporary BLE provisioning transport and
  credential handoff.
- `app_gui` owns reset-result rendering and presentation acknowledgment.
- `main` remains the composition root.

## Regression Closure Record

The following areas are closed by user acceptance of the integrated Sprint 7
behavior:

- [x] Short press does not erase configuration.
- [x] Long press produces at most one reset transaction per press cycle.
- [x] Successful reset clears both persistent Wi-Fi copies.
- [x] Persistent state is verified as `NOT_CONFIGURED` before reboot.
- [x] Reset success is presented through the GUI-owned path before the
      controlled restart or bounded fallback.
- [x] Reset failure remains non-rebooting and can be re-armed by release.
- [x] Active provisioning is quiesced before persistent erasure.
- [x] Automatic reconnect and late connection adoption cannot win after the
      reset gate is asserted.
- [x] The device returns to BLE provisioning after reboot.
- [x] Reprovisioning can obtain IPv4 without `erase-flash`.
- [x] No new production task, queue, lock, timer, or state-machine branch is
      added by Phase 7.6 closure work.

This record reflects hardware/manual acceptance confirmed by the user. It does
not claim that an automated fault-injection suite or CI hardware farm executed
every theoretical interleaving.

## Runtime Resource Baseline

A representative accepted runtime snapshot at approximately 318 seconds was:

| Resource | Current | Minimum / largest observation |
|---|---:|---:|
| CPU used | 3.8% | 96.1% idle across two cores |
| Internal RAM free | 27,551 bytes | minimum 7,443; largest block 10,752 |
| PSRAM free | 8,380,560 bytes | minimum 8,352,180; largest block 8,257,536 |
| DMA-capable RAM free | 19,763 bytes | minimum 1,391; largest block 10,752 |
| `perf_monitor` stack remaining | 4,584 bytes | high-water measurement |

The baseline is sufficient to close Sprint 7. DMA-capable low-water headroom is
still narrow enough to keep monitoring during later TLS, audio, and GUI work.
RAM optimization is intentionally deferred until a measured allocation problem
or a later resource-heavy sprint requires it.

## Known Observation Deferred to Sprint 8

An intermittent TLS certificate-bundle failure has been observed:

```text
esp-x509-crt-bundle: Failed to verify certificate
mbedtls_ssl_handshake returned -0x3000
```

The cloud state correctly enters retry for this transport failure. Root-cause
work for intermittent TLS, retry diagnostics, allocation-failure evidence, and
cloud robustness belongs to Sprint 8. Phase 7.6 does not claim that this cloud
issue is resolved.

## Documentation and Skeleton Closure

Phase 7.6 performs documentation-only cleanup:

- Record Sprint 7 completion and accepted ownership boundaries.
- Remove stale statements that Phase 7.4 or Phase 7.5 hardware acceptance is
  pending.
- Refresh public API docstrings without changing declarations or behavior.
- Apply `skeleton_file.md` section naming to touched source/header files.
- Preserve executable statements, declarations, configuration values, timing,
  task behavior, and component ownership unchanged.

For existing files, empty skeleton sections are not added merely as
placeholders. A section may be omitted when that category has no content, as
allowed by `skeleton_file.md`.

## Validation of This Closure Change

- Documentation review: performed.
- Comment/docstring review: performed.
- Executable code modification: none.
- Build: not rerun because this closure intentionally changes no executable
  code or build configuration.
- Automated tests: not run for the documentation-only closure.
- Hardware acceptance: confirmed by the user.

## Final Sprint 7 State

```text
Phase 7.1 — COMPLETE
Phase 7.2 — COMPLETE
Phase 7.3 — COMPLETE
Phase 7.4 — COMPLETE
Phase 7.5 — COMPLETE
Phase 7.6 — COMPLETE
Sprint 7   — COMPLETE
```

The next planned work is Sprint 8 network/cloud robustness. This document does
not start or implement Sprint 8.
