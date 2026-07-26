# AGENTS.md — ESP32-S3 Smart Room Cloud Gateway

## Project Role
You are assisting with an ESP-IDF embedded project for ESP32-S3.
The goal is to build a practical IoT device with:

- BLE Wi-Fi provisioning
- Wi-Fi station mode
- LVGL LCD dashboard
- Sensor monitoring
- Firebase Realtime Database logging
- NVS configuration storage
- Button factory reset

This is a learning-oriented embedded project. Do not over-engineer or implement features outside the requested sprint.

---

## Coding Principles

1. Prefer ESP-IDF style C code unless explicitly requested otherwise.
2. Keep changes minimal and scoped to the current task.
3. Do not rewrite unrelated files.
4. Do not add Wi-Fi, BLE, Firebase, MQTT, OTA, or web server code unless the task explicitly asks for it.
5. Use component-based structure.
6. Add clear `ESP_LOGI/W/E` logs for init, state changes, and errors.
7. Always check and return `esp_err_t` where appropriate.
8. Avoid blocking forever inside component APIs unless explicitly designed as a task loop.
9. Avoid calling LVGL APIs from random tasks. UI updates should go through a UI manager/task or a clearly controlled function.
10. Keep memory usage reasonable. Avoid large static buffers unless necessary.

---

## Preferred Project Structure

```text
components/
├── cloud/
│   ├── cloud_manager/
│   └── firebase_auth/
├── connectivity/
│   ├── provisioning_manager/
│   └── wifi_manager/
├── display/
│   ├── display_driver/
│   └── waveshare__esp_lcd_st7735/
├── sensing/
│   ├── sensor_manager/
│   └── sensor_DHT22/
├── storage/
│   ├── config_manager/
│   └── sd_card_manager/
├── system/
│   ├── common/
│   └── performance_monitor/
└── ui/
    ├── app_gui/
    ├── ui_manager_lvgl/
    ├── lvgl_image_handler/
    └── lvgl_sd_fs/
```

The first-level folders are organizational domains. Their children remain
independent ESP-IDF components with separate public APIs. Only create
components required by the current sprint.

---

## Current MVP Scope

MVP target:

```text
BLE Wi-Fi Provisioning
+ Wi-Fi Station
+ LVGL LCD Dashboard
+ Sensor Monitor
+ Firebase Realtime Database Logging
+ NVS Config Storage
+ Button Factory Reset
```

Out of scope for early sprints:

```text
Custom mobile app
Firestore direct integration
OTA
MQTT
WebSocket dashboard
BLE always-on data streaming
Complex animation/UI theme
```

---

## Sprint Order

### Sprint 0 — Project Setup
Goal:
- Create clean ESP-IDF project.
- Confirm build/flash/monitor works.
- Add basic component structure only if needed.
- Add clear README skeleton.

Do not implement LVGL/Wi-Fi/BLE/Firebase yet.

### Sprint 1 — LCD + LVGL Bring-up
Goal:
- Initialize LCD display.
- Initialize LVGL.
- Show a simple screen.
- Update one counter/status label periodically.

Out of scope:
- Wi-Fi
- BLE
- Firebase
- Sensor
- NVS

### Sprint 2 — Wi-Fi Hardcoded + LVGL Status
Goal:
- Connect Wi-Fi using hardcoded credentials or menuconfig.
- Display Wi-Fi state/IP/RSSI on LVGL screen.

### Sprint 3 — Sensor + UI
Goal:
- Read sensor periodically.
- Send sensor event to UI.
- Update LVGL labels safely.

### Sprint 4 — Firebase Realtime Database
Goal:
- Upload latest sensor JSON through HTTPS REST.
- Show cloud sync state on LVGL.

### Sprint 5 — NVS + BLE Provisioning
Goal:
- Store Wi-Fi credentials in NVS.
- Use BLE provisioning to receive credentials.
- Connect Wi-Fi after provisioning.
- Stop BLE after successful provisioning.

### Sprint 6 — Factory Reset + Polish
Goal:
- Long press button clears Wi-Fi config.
- Reconnect strategy.
- Error states.
- Documentation and demo preparation.

---

## Task Response Requirements

After making changes, always summarize:

1. Files changed
2. What was implemented
3. What was intentionally not implemented
4. How to build
5. How to flash/monitor
6. Expected serial log/output
7. Risks or follow-up tasks

---

## Build Commands

Use typical ESP-IDF commands:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Do not assume the COM port. Ask the user to provide it or leave `<PORT>` placeholder.

---

## Hardware Assumptions

Target board:
- ESP32-S3, likely N16R3 variant

Potential peripherals:
- ST7735 LCD 128x160 over SPI
- DHT22 or similar temperature/humidity sensor
- Button for factory reset
- LED or RGB LED for status

Do not assume exact pins unless the user provides them.

---

## Quality Bar

The code should be:

- Buildable
- Small enough to review
- Easy to debug
- Properly logged
- Suitable for learning
- Not overly clever

If a requested implementation depends on missing hardware pins, ESP-IDF version, or library choice, clearly state the assumptions and keep the implementation easy to adjust.

---

# END PHASE Automation Protocol

## Command Detection

Treat any user message matching:

```text
END PHASE <phase_id>
```

as confirmation that implementation and manual or hardware acceptance for that
checkpoint have passed, unless the user explicitly says otherwise.

For example, `END PHASE 6.3.2` instructs Codex to:

- review the completed checkpoint;
- clean its code;
- improve its documentation;
- validate the repository;
- prepare a Git-ready checkpoint.

This command does not authorize a Git commit, push, merge, pull request,
destructive Git operation, or implementation of the next phase. Never commit,
push, merge, reset, discard, checkout, or delete work unless the user
explicitly requests that action.

## 1. Inspect Before Editing

When an `END PHASE` command is received:

1. Run or inspect `git status`.
2. Inspect the current diff.
3. Identify files related to `<phase_id>`.
4. Read the relevant roadmap, tracker, README files, component documentation,
   and recent implementation.
5. Preserve unrelated and potentially uncommitted work.
6. Never overwrite or clean unrelated files.
7. Prefer focused edits over full-file rewrites.

Infer phase scope from the phase identifier, current repository changes,
roadmap or tracker, component documentation, and implementation context. Do
not introduce unrelated features.

## 2. Review Correctness

Review the phase implementation for:

- functional correctness and return-value handling;
- error paths, cleanup, lifecycle, and state-machine correctness;
- concurrency, race conditions, and deadlock risks;
- memory ownership, lifetime, and buffer bounds;
- timeout behavior;
- task and callback ownership;
- security and sensitive-data handling;
- compatibility with the existing architecture.

For ESP-IDF and FreeRTOS, also verify:

- ISR-safe APIs are used only from ISR context and task APIs from task context;
- no blocking operation runs inside a critical section;
- no LVGL call is made from an arbitrary callback or non-UI task;
- callbacks remain short;
- shared state is synchronized correctly;
- queues, mutexes, timers, event groups, and task notifications have clear
  ownership;
- waits that can fail use finite timeouts;
- tasks have documented stack size, priority, purpose, and lifecycle;
- temporary credential buffers are securely cleared;
- credentials, passwords, PoP values, tokens, and secrets are never logged.

Fix only confirmed problems within the phase scope. Do not perform speculative
architecture rewrites during phase cleanup.

## 3. Clean Phase Code

Clean only code related to `<phase_id>`. Remove or fix:

- temporary bring-up and fault-injection code;
- obsolete test-only macros and temporary diagnostic loops;
- debug log spam and hardcoded Wi-Fi credentials;
- stale TODO comments and prototypes;
- unused includes, variables, and macros;
- duplicated logic and unreachable state branches;
- outdated comments;
- commented-out executable code without a clear future purpose;
- formatting inconsistencies introduced by the phase.

Preserve reusable test utilities only when clearly isolated and documented. Do
not remove useful production diagnostics.

## 4. Improve Code Documentation

Update public API documentation with relevant:

- purpose, parameters, and return values;
- prerequisites;
- blocking or non-blocking behavior;
- timeout behavior;
- thread safety and ISR, callback, or task context;
- ownership and lifetime;
- security constraints.

Add private function comments only for non-obvious lifecycle behavior, state
transitions, concurrency requirements, ownership transfer, cleanup
requirements, framework workarounds, or architectural reasoning. Do not add
comments that merely repeat the code; explain why a rule exists.

## 5. Update Documentation

Update all relevant documentation when present:

- component `docs/README.md`;
- component `README.md`;
- root `README.md`;
- project roadmap and sprint tracker;
- architecture notes and project-state documents.

Documentation must describe the final implementation rather than the original
plan. Update relevant purpose, ownership, dependencies, public APIs,
initialization, state-machine, task/callback/queue/event flow, timeout/retry,
threading, memory ownership, credentials, security, known limitations,
manual/hardware acceptance, and intentionally deferred work.

Remove outdated claims, including completed functionality still marked as not
implemented, obsolete API behavior, temporary bring-up descriptions, and
incorrect GPIO, timing, task, or lifecycle values.

Mark only `<phase_id>` complete. Do not mark a parent phase complete unless
every required child checkpoint is complete. For example:

```text
6.3.2 Dedicated coordinator task - COMPLETE
```

does not by itself authorize marking Phase 6.3 complete.

## 6. Preserve Project Architecture

Preserve these ownership boundaries unless the current phase explicitly
changes them:

- `main` is the composition root.
- `config_manager` owns persistent application configuration.
- `wifi_manager` owns Wi-Fi Station connection and reconnect behavior.
- `provisioning_manager` owns temporary BLE provisioning transport.
- `app_network_coordinator` owns application-level network orchestration.
- `app_gui` owns GUI screens, models, and UI queues.
- `ui_manager_lvgl` owns LVGL runtime and synchronization.
- Callbacks must not call LVGL directly.
- Persistent Wi-Fi credentials are written only through `config_manager`.
- Configuration locks and NVS handles are released before calling Wi-Fi
  connection APIs.
- Sensitive temporary buffers are cleared after use.
- Network, GUI, sensor, storage, and cloud components do not take over one
  another's responsibilities.

Apply the smallest safe fix when an ownership violation is confirmed.

## 7. Validate

Run validation appropriate for the repository. For ESP-IDF, normally run:

```text
idf.py build
```

Also run relevant existing unit tests, integration tests, static checks,
formatting checks, and repository validation scripts.

Do not claim hardware testing was performed unless the user already confirmed
it or actual hardware logs are available. Clearly distinguish:

- build performed;
- automated tests performed;
- hardware or manual acceptance confirmed by the user;
- checks that could not be run.

Do not hide warnings or failures. Do not modify unrelated code merely to
remove unrelated warnings.

## 8. Final Diff Inspection

Before reporting completion:

1. Review the complete phase-related diff.
2. Confirm no unrelated file was modified accidentally.
3. Confirm no password, credential, token, private key, PoP, or secret was
   added.
4. Confirm documentation matches implementation.
5. Confirm the repository builds when build tools are available.
6. Confirm the checkpoint can be reverted independently.
7. Confirm temporary phase code has been removed.
8. Confirm no destructive Git operation was executed.

## 9. Completion Report

After processing `END PHASE`, return this compact report:

```text
# END PHASE <phase_id> - Ready For Commit

## Status

- Cleanup: PASS / PARTIAL / BLOCKED
- Build: PASS / FAIL / NOT RUN
- Automated tests: PASS / FAIL / NOT RUN
- Hardware acceptance: CONFIRMED BY USER / NOT CONFIRMED

## Code Cleaned

- List important cleanup changes.

## Documentation Updated

- List updated documentation.

## Correctness Fixes

- List meaningful fixes.
- Write `None` when no correctness problem was found.

## Known Limitations

- List intentionally deferred work.

## Files Changed

- List changed files grouped by component.

## Validation

- List commands run and their results.

## Suggested Commit Message

<type>(<scope>): <engineering summary> [<phase_id>]

## Suggested Commit Body

- Provide 2-6 concise bullets covering implementation, cleanup,
  documentation, and validation.

## Next Checkpoint

State the next planned checkpoint, but do not implement it automatically.
```

Use an engineering-specific commit subject, for example:

```text
feat(network): run coordinator boot policy in dedicated task [6.3.2]
```

Do not suggest a vague subject such as `End phase 6.3.2`.

## 10. Blocking Conditions

Report `BLOCKED` instead of pretending completion when:

- phase-related code does not build;
- a critical lifecycle, concurrency, memory, or security issue remains;
- required files are missing;
- tracked source contains credentials or secrets;
- documentation contradicts implementation;
- cleanup would overwrite unrelated changes;
- phase acceptance criteria have not actually been met.

When blocked:

- explain the exact blocker;
- identify affected files;
- recommend the smallest safe fix;
- do not mark the phase complete;
- do not start the next phase.

## Final Rule

`END PHASE <phase_id>` must produce a clean, documented, validated,
reviewable, and Git-ready checkpoint while preserving unrelated work. Do not
sacrifice correctness, architecture, security, or repository safety merely to
mark a phase complete.
