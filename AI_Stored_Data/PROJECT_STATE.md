# Smart Room Cloud Gateway — AI Project State

Updated from branch: `main_including_Firebase_security`
Snapshot date: 2026-09-12
Production/source baseline before the documentation-sync commits: `7a74086b8211aff635a2651cbc54edab014a8920` (`fix security`)

## Working Constitution

- `AGENTS.md` is the repository-specific operating guide.
- Preserve completed roadmap history and phase boundaries.
- Inspect implementation and documentation before editing.
- Keep changes evidence-based.
- Never claim build, HIL, merge, or runtime success without evidence.
- `AI_Stored_Data/` is cross-session support metadata only and may be deleted; firmware/build code must never depend on it.
- Do not commit, push, merge, reset, discard, or delete repository work unless the user explicitly authorizes that action. Updates inside `AI_Stored_Data/` are allowed when Hải asks to synchronize this handoff store.

## Current branch anchor

The requested integration branch is:

```text
main_including_Firebase_security
```

Production/source history relevant to this snapshot:

```text
b7ef51a87dcefa330cd0aa42e4d52dafe60f2bba
  Merge component portability hardening into main_including_Firebase_security

1c26433364dd75fc2b1537c2a50fa35468c07cf1
  Add firebase key to gitignore

7a74086b8211aff635a2651cbc54edab014a8920
  fix security
```

The `fix security` commit replaces several unbounded `strcpy` calls in
`log_manager` and its host test with bounded `snprintf`. This snapshot records
the source change only; it does not claim a new firmware build or HIL run for
that commit.

Documentation-only synchronization commits may advance the branch HEAD beyond
`7a74086b...`; do not mistake those metadata/documentation commits for a newer
production validation baseline.

## Current high-level state

```text
Sprint 12   Software complete / HIL PASS
Sprint 13   Software complete / HIL PASS
Sprint 14   Software complete / BUILD PASS / golden-path HIL PASS / targeted regression partial
Sprint 15   COMPLETE / BUILD VERIFIED / HIL ACCEPTED
Sprint 16   Audio Arbitration / SOFTWARE COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / BOUNDED HIL ACCEPTED
Phase 16.1  PCM streaming downlink IMPLEMENTED / BUILD VERIFIED / automated HIL PASS / audible recovery confirmed
Sprint 17   MCP Read-Only Tools / IN PROGRESS; sensor answer and cloud-sync status HIL accepted by user
Sprint 18   MCP Controlled Actions / NOT STARTED
Sprint 19   Wake Word And Advanced Voice UX / NOT STARTED
Major feature-coding stage through Phase 16.1 COMPLETE; later feature phases require explicit start
```

Authoritative Phase-16 closure: `AI_Stored_Data/PHASE16_PROGRESS.md`.
Phase-16 HIL plan: `AI_Stored_Data/PHASE16_HIL_TEST_PLAN.md`.
Phase-16 HIL evidence: `AI_Stored_Data/PHASE16_HIL_EVIDENCE.md`.
Phase-16.1 streaming record: `AI_Stored_Data/PHASE16_1_STREAMING_DOWNLINK.md`.
Canonical detailed Version-2 phase numbering: `XIAOZHI_IMPLEMENTATION_ROADMAP.md`.

## Roadmap numbering reconciliation

The previous roadmap draft used Sprint 16 for MCP read-only. That numbering is
superseded because Sprint 16 and Phase 16.1 now have completed, accepted
implementation history that must not be renumbered.

Use this sequence from now on:

```text
Sprint 16   Audio Arbitration & Multi-Client Audio Policy       COMPLETE
Phase 16.1  Xiaozhi PCM Streaming Downlink                     COMPLETE
Sprint 17   MCP Read-Only Tools                                IN PROGRESS
Sprint 18   MCP Controlled Actions                             NOT STARTED
Sprint 19   Wake Word And Advanced Voice UX                    NOT STARTED
```

Interpretation rule:

- "read-only MCP" = **Sprint/Phase 17**;
- "controlled MCP actions" = **Sprint/Phase 18**;
- "wake word / advanced voice UX" = **Sprint/Phase 19**;
- never reuse Phase 16 for MCP work;
- never rewrite Phase-16/16.1 closure evidence merely to match an older draft.

`XIAOZHI_IMPLEMENTATION_ROADMAP.md` and `docs/KNOWN_LIMITATIONS.md` were
reconciled to this numbering on 2026-09-09. If an older historical roadmap
snapshot still shows the old 16/17/18 mapping, this reconciliation supersedes
that numbering only; preserve the historical implementation content.

## Component portability hardening integration

The portability-hardening implementation is no longer an unmerged side branch.
It was integrated into `main_including_Firebase_security` by merge commit:

```text
b7ef51a87dcefa330cd0aa42e4d52dafe60f2bba
```

Current status:

```text
Architecture direction                 DONE / FROZEN
Agreed refactor implementation         DONE / FROZEN
Private-module/dependency documentation DONE / FROZEN
Merge into main_including_Firebase_security COMPLETE
Full post-refactor automated acceptance PENDING unless newer evidence is recorded
Target-board portability smoke acceptance PENDING unless newer evidence is recorded
```

Do not start another portability or component-genericization wave merely to
improve an abstract portability score. If validation finds a concrete regression,
identify the failing component/path and apply the smallest safe correction.

Important integrated outcomes include:

- domain-grouped ESP-IDF component layout retained;
- large tightly coupled implementations organized as parent-owned private
  `modules/` rather than many artificial child components;
- dependency direction hardened toward `application -> service -> driver/framework`;
- public CMake dependency surfaces separated from implementation-only dependencies;
- `sensor_DHT22` receives GPIO through runtime configuration;
- SD path-size ownership moved into `sd_card_manager` API ownership;
- selected external-RAM task-stack handling uses explicit ESP-IDF support;
- Firebase development configuration is sourced through project Kconfig/menuconfig
  rather than hard-coded application constants;
- product/application components remain intentionally product-specific rather
  than being generalized without a real reuse requirement.

Detailed historical record: `AI_Stored_Data/COMPONENT_PORTABILITY_HARDENING.md`.

## HIL routing

```text
RUN PHASE 12 HIL -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL -> test/phase14-ptt-voice-e2e-hil
RUN PHASE 15 HIL -> test/phase15-voice-ui-hil
RUN PHASE 16 HIL -> test/phase16-audio-arbitration-hil
RUN PHASE 16.1 HIL -> phase/16.1-streaming-downlink
```

Inspect the worktree and route to the dedicated historical test branch before an
older-phase HIL run. Never silently use arbitrary latest production source for
historical acceptance.

## Established system ownership

- `main`: application composition root.
- `config_manager`: persistent application configuration owner.
- `wifi_manager`: Wi-Fi Station connection and reconnect owner.
- `provisioning_manager`: temporary BLE provisioning transport owner.
- `app_network_coordinator`: application-level network orchestration owner.
- `audio_manager`: sole microphone, speaker, I2S, DMA, PCM-buffer, and playback/capture resource owner.
- `xiaozhi_foundation`: sole direct managed `esp_xiaozhi` provider boundary; provider handles/credentials must not escape.
- `voice_assistant`: long-lived product voice session/recovery orchestration.
- `voice_assistant_ptt`: PTT authorization policy.
- `voice_assistant_ui_model` plus adapter: copied lifecycle, text, and capture-presentation path.
- `app_log`: reusable logging frontend.
- `log_manager`: optional persistent logging backend/storage policy.
- `app_gui`: GUI screens, copied models, and UI queues.
- `ui_manager_lvgl`: LVGL runtime and synchronization owner.
- `sd_card_manager`: SD lifecycle/lease owner.
- GPIO9: factory reset only.
- GPIO38: PTT input, internal pull-down, active-high.
- GPIO48: NeoPixel reservation; never use it as PTT.

`components/system/common/include/board_config.h` remains the current hardware
mapping source of truth.

## Component organization contract

Top-level domain folders are organizational containers. Their direct children
are ESP-IDF components. A large component may contain private `modules/<name>/`
subsystems owned by the parent component.

Rules to preserve:

1. Parent CMake owns private-module sources and private include paths.
2. Private modules do not become standalone ESP-IDF components by default.
3. Other components must not include another component's private `modules/`
   headers.
4. Promote a private module only when it gains a real independent reuse or
   lifecycle requirement.
5. Lower-level reusable components must not depend on application coordinators or
   `app_gui`.
6. A dependency exposed by a public header belongs in public CMake dependency
   visibility; implementation-only dependencies remain private.

## Phase 14/15 voice path and Phase 16.1 downlink

```text
PTT
-> Xiaozhi READY
-> INMP441 / audio_manager capture
-> copied PCM16 uplink
-> Xiaozhi response
-> copied bounded downlink queue
-> Opus decode worker
-> audio_manager-owned PSRAM PCM16 ring
-> manager-owned streaming playback
-> MAX98357
```

Phase 14 retains the production Xiaozhi session through unexpected disconnects,
protects response ownership by session generation, rejects PTT while a response
is awaiting/collecting/finalizing/playing, and bounds response wait to 15 seconds
inactivity / 600 seconds total.

Phase 16.1 replaces response aggregation plus SD/WAV handoff with a 7.68-second
bounded PCM16 ring and a 1.44-second streaming prefill. The downlink worker
decodes and copies complete frames while `audio_manager` remains the only
I2S/DMA owner. A post-start dry ingress supplies explicit silence for up to
eight seconds so transient jitter does not replay the prior DMA block. The
automated target matrix passed and audible recovery was confirmed; endurance
remains pending.

Phase 15 adds copied lifecycle plus USER/ASSISTANT text presentation through the
UI task. The UI reports `RECORDING` only when actual microphone capture is
active, renders a live `RECORD` duration during capture, and freezes the duration
afterward. Exact CONNECTING/THINKING/RECOVERING labels remain intentionally
coalesced by the reused legacy GUI surface. Phase-15 HIL acceptance was
confirmed by the user on 2026-09-06; the unattended target lifecycle regression
passed 7/7 at `fc5a3fa`. That suite verifies state/queue/timer routing, while
physical LCD/audio/GPIO and real semantic-text observations remain
operator-confirmed evidence.

## Phase 16 — audio arbitration architecture

Logical audio clients request resources through project-owned metadata:

```text
audio_manager_request_t
├── request_id
├── client
├── resource = CAPTURE / PLAYBACK
├── priority
├── busy_policy = REJECT / QUEUE / PREEMPT_LOWER_PRIORITY
└── interruptible
```

Known clients include SYSTEM, XIAOZHI, NOTIFICATION, ALARM, RECORDER, UI, and
TEST. No client receives an I2S handle, DMA buffer, raw source handle, or direct
hardware ownership.

### Playback

```text
client
-> playback arbiter
-> public audio_manager playback control
-> manager task
-> sole I2S TX
```

One current and one pending playback request are bounded. WAV requests retain
their SD-prefetch path; one PCM16 stream request owns a separate bounded PSRAM
ingress session. ACTIVE requires real PLAYBACK evidence, not command acceptance.
A known interruptible lower-priority playback may be cooperatively stopped
through `audio_manager_stop_playback()`.

### Capture

```text
client
-> capture arbiter
-> audio_manager_start_recording()
-> manager task
-> sole I2S RX
```

One current and one pending request are bounded. ACTIVE requires real RECORDING
evidence. PROCESSING is allowed to finish naturally before pending promotion.
Cooperative stop uses `audio_manager_stop_recording()`.

### Deterministic policy

```text
no known owner -> GRANT
REJECT -> REJECT
QUEUE -> WAIT
PREEMPT_LOWER_PRIORITY -> PREEMPT only when incoming priority is strictly higher and current owner is interruptible
same/lower priority -> never preempt
```

Unknown legacy/external manager activity is never preempted because trusted
client metadata is absent. Capture and playback arbiters are separate but share
the one `audio_manager` operation state; manager serialization remains the final
hardware gate. Global cross-resource fairness is intentionally not claimed.

### Xiaozhi, notification, and alarm policy

```text
XIAOZHI      priority=70  CAPTURE=REJECT  PLAYBACK=QUEUE  interruptible=true
NOTIFICATION priority=50  PLAYBACK=QUEUE  interruptible=true
ALARM        priority=100 PLAYBACK=PREEMPT_LOWER_PRIORITY interruptible=false
```

Phase 16.1's Xiaozhi downlink reserves the bounded stream through the playback
arbiter at TTS_START. It begins I2S only after a 1.44-second PCM prefill or a
short-response EOS, then drains asynchronously after TTS_STOP. A full ingress
ring retains and retries the same decoded packet through a finite backpressure
window rather than dropping it. The arbiter retains terminal state per request
so cancellation, alarm preemption, and stream failures cannot be misreported as
normal completion. A temporary empty ingress writes explicit silence rather
than allowing I2S/DMA to repeat its preceding block, with an eight-second
bounded recovery window.

## Security/configuration notes

- Real Firebase development values belong in local generated configuration, not
  tracked source/documentation.
- `.FireBaseKey` is ignored by the repository as of commit
  `1c26433364dd75fc2b1537c2a50fa35468c07cf1`.
- Never place Firebase passwords/tokens, Wi-Fi credentials, PoP values, private
  keys, service-account JSON, or local secret payloads in `AI_Stored_Data/`.
- A source-level safety cleanup in `7a74086b...` replaced several remaining
  unbounded `strcpy` calls in `log_manager`; validation of that exact commit must
  be reported separately when actually performed.

## Known pending acceptance / technical debt

1. Phase-16 bounded target HIL is accepted; endurance and broader full-integration regression remain deferred.
2. Phase-15 visible LCD/text HIL remains partial: `RECORDING` timer, USER/ASSISTANT text, latest-turn behavior, recovery presentation, truncation, and UI-resource evidence need target confirmation.
3. Phase-14 fault-injection cases remain deferred; the exact regression image still needs fresh audible speaker confirmation.
4. Phase-16.1 automated target matrix and audible recovery are accepted. Endurance coverage remains pending.
5. Long-duration Firebase/cloud plus Xiaozhi simultaneous-traffic regression remains deferred.
6. Playback/capture arbiters have no dedicated stop/deinit lifecycle API yet; recorded-audio playback is not migrated to arbitration.
7. Cross-resource global audio fairness is not guaranteed by the separate arbiters.
8. Notification/alarm priority/preemption passed the bounded target matrix; repeated timing/endurance coverage remains deferred.
9. Full post-portability-refactor automated and target-board smoke acceptance remains pending unless newer explicit evidence supersedes this snapshot.

## Next-work guidance

1. Treat `main_including_Firebase_security` as the current integrated baseline for new work unless Hải explicitly chooses another branch.
2. Retain historical HIL branches as regression baselines rather than rewriting their acceptance history.
3. Finish the still-pending Phase-15 visible UI/text acceptance when relevant.
4. Treat Phase 12 and Phase 13 as closed regression baselines; retain Phase 14's recorded golden-path result and run only relevant regressions.
5. Complete the post-portability-refactor build/host-test/target-smoke acceptance before reopening architecture work.
6. Run full Gateway/Firebase + voice integration regression, bug fixing, hardening, performance/resource validation, documentation, and release/portfolio closure.
7. Do not start Phase 17 or another major feature phase automatically. If Hải explicitly starts Phase 17, its scope is MCP Read-Only Tools.
