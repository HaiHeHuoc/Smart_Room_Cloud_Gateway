# Phase 15 Voice Assistant UI / Conversation Presentation Progress

Updated: 2026-09-06
Branch: `phase/15-voice-assistant-ui`
Current checkpoint: **15-F — FINAL Review / Closure**
Status: **COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / HIL ACCEPTED**

## Collaboration result

Phase 15 was implemented checkpoint-by-checkpoint and is now software-closed. Do not start Phase 16 automatically; wait for explicit user direction.

Completed checkpoints:

1. 15-A — production voice event/UI model. ✅
2. 15-B — Voice Assistant screen/lifecycle presentation. ✅
3. 15-C — production USER transcript wiring. ✅
4. 15-D — production ASSISTANT text wiring. ✅
5. 15-E — repeated-turn/history/error UX. ✅
6. 15-F — final review/composition/docs/deferred HIL. ✅

## Final production presentation architecture

```text
voice_assistant lifecycle
          +
Xiaozhi CHAT_TEXT semantic events
          ↓
voice_assistant_ui_model
  - copied state
  - session_generation
  - presentation turn_sequence
  - bounded USER text
  - bounded ASSISTANT text
  - error/truncation flags
          ↓
voice_assistant_ui_gui_adapter
          ↓
app_gui copied latest-value queue
          ↓
app_gui UI task
          ↓
LVGL Voice/Xiaozhi visual surface
```

No Xiaozhi/voice callback calls LVGL directly.

## Production semantic text path

The project lock pins `espressif/esp_xiaozhi` 0.1.2. Phase 15 observes the semantic `CHAT_TEXT` contract and classifies USER/ASSISTANT roles through a dedicated `xiaozhi_foundation` text boundary.

The upstream `event_data`/text pointer is callback-lifetime only. The project does not retain it. The production UI model copies text synchronously into bounded 192-byte fields before callback return.

Observer ownership remains distinct:

```text
xiaozhi session status
    -> voice_assistant

xiaozhi binary/TTS response
    -> voice_assistant_downlink

xiaozhi semantic USER/ASSISTANT text
    -> voice_assistant_ui_model
```

## USER / ASSISTANT behavior

USER semantic text:

```text
USER event
-> generation check
-> increment presentation turn_sequence
-> clear assistant text from prior displayed turn
-> replace USER text
-> publish copied model
```

ASSISTANT semantic text:

```text
ASSISTANT event
-> generation check
-> retain current USER text
-> replace ASSISTANT text
-> publish copied model
```

Assistant text is intentionally independent from Phase-14 response audio playback and may appear before/during speaker playback.

## Repeated-turn policy

Phase 15 intentionally uses a **latest-turn view**, not unbounded conversation history. This matches the 160x128 LCD and avoids unbounded RAM growth.

`session_generation` identifies a long-lived Xiaozhi session and can span multiple PTT turns. `turn_sequence` is therefore presentation-only and increments for each accepted USER semantic event in the current session.

When a new non-zero session generation appears:

```text
clear USER text
clear ASSISTANT text
turn_sequence = 0
```

Stale old-generation text remains rejected.

## Error / recovery UX

ERROR/RECOVERING do not erase the latest conversation text. The user keeps visible context while lifecycle recovery is shown.

### Final-review software defect found and fixed

15-F found a real adapter/legacy-GUI contract mismatch:

- production RECOVERING may legitimately carry `last_error != ESP_OK`;
- the Phase-15 adapter maps RECOVERING to the reused legacy `PROCESSING` visual state;
- `app_gui` accepts non-OK `last_error` only when its visual state is `ERROR`;
- therefore a valid RECOVERING snapshot could be rejected before reaching LVGL.

Fix: `voice_assistant_ui_gui_adapter` now normalizes only the **legacy translated snapshot**:

```text
legacy visual != ERROR -> last_error = ESP_OK
legacy visual == ERROR -> preserve non-OK production error,
                          fallback ESP_FAIL if needed
```

The production `voice_assistant_ui_model` still retains the real error value. This preserves diagnostic semantics while satisfying the existing `app_gui` validation contract.

Fix commit: `d0d7a5b7b4fd1c52e163a917050420289ccd1684`.

## Production composition / validation isolation

Normal production startup remains:

```text
audio_manager READY
-> voice_assistant
-> voice_assistant_ui_model
-> voice_assistant_ui_gui_adapter
-> PTT policy
-> uplink
-> downlink
-> PTT GPIO
-> queue one long-lived Xiaozhi service session
```

When `CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE=y`, the Phase-14/15 production voice stack remains suppressed. Phase-12 validation and Phase-15 production presentation therefore do not intentionally run as competing application owners.

## Xiaozhi semantic init bridge

`xiaozhi_session.c` has only its `esp_xiaozhi_chat_init()` symbol source-locally redirected through `xiaozhi_foundation_chat_init_bridge()`.

The bridge preserves the original production callback/context, forwards all events unchanged, and additionally publishes CHAT_TEXT semantic events. This is a controlled integration seam rather than a transport rewrite.

The merged Phase-15 production branch built with ESP-IDF 6.0.1 on 2026-09-02
(binary `0x21cdd0`, 47% app partition free). The source-local bridge compiled
successfully against pinned `esp_xiaozhi` 0.1.2 alongside the bounded public
WebSocket-send wrappers. Refactor to a direct/public integration point later
only if maintenance/build evidence shows the seam is fragile.

## Final presentation-state decision

Phase 15 production model keeps exact states:

```text
IDLE
CONNECTING
READY
LISTENING
THINKING
SPEAKING
RECOVERING
ERROR
```

The reused Phase-12 `app_gui` surface still renders:

```text
CONNECTING -> PROCESSING
THINKING   -> PROCESSING
RECOVERING -> PROCESSING
READY      -> READY
LISTENING  -> RECORDING (only while microphone capture is active)
SPEAKING   -> RESPONDING
ERROR      -> ERROR
```

The UI model now derives `LISTENING` from the actual `VOICE_ASSISTANT_AUDIO_RECORDING` and `capture_active` values, rather than the button authorization alone. It timestamps capture start/stop with the monotonic ESP timer; the UI task renders `RECORD <duration>` while capture is active and preserves the final `LISTEN <duration>` after it stops. This is **build verified**, but visible LCD evidence remains a Phase-15 HIL requirement.

This is accepted as a documented **MVP presentation limitation**, not an architecture blocker. Do not claim exact CONNECTING/THINKING/RECOVERING LCD labels. A later small GUI refinement may split those labels after build/HIL evidence if worthwhile.

## Phase-15 HIL plan

Created:

`AI_Stored_Data/PHASE15_HIL_TEST_PLAN.md`

Recommended future test branch:

`test/phase15-voice-ui-hil`

Suggested activation label:

`RUN PHASE 15 HIL`

The plan covers:

- clean build/link;
- boot/composition and Phase-12 validation isolation;
- Voice screen routing;
- real USER transcript;
- real ASSISTANT text independent from audio;
- at least three repeated turns;
- new-session stale-text cleanup;
- ERROR/RECOVERING presentation and the final-review error-contract fix;
- text truncation robustness;
- LVGL ownership/UI stress;
- memory/stack/queue resource trend.

## Intentionally out of scope / future enhancement

- full scrollable conversation history;
- waveform/audio-level visualization;
- GUI PTT button;
- exact legacy app_gui state-enum rewrite;
- new hardware behavior;
- general multi-client audio arbitration (already tracked as a post-Phase-14 architecture follow-up).

## Targeted target evidence — 2026-09-02

The target serial trace confirms the boot-queued session reaches `READY`, an
unexpected disconnect returns through `CONNECTING` to `READY`, and one GPIO38
turn reaches actual capture, `response WAIT`, playback request and
`PLAYBACK_COMPLETE`. A GPIO38 press during that response wait was rejected
before another recording starts.

This trace was **targeted transport/audio HIL** and was not, by itself, the
Phase-15 UI/text acceptance evidence.

## HIL acceptance — 2026-09-06

The Phase-15 hardware/manual acceptance was confirmed by the operator through
`ENDPHASE 15`. The current target-derived UI lifecycle regression also passed
unattended on `test/xiaozhi-ui-lifecycle-hil` at `fc5a3fa`:

```text
XIAOZHI_UI_HIL SUMMARY pass=7 fail=0
XIAOZHI_UI_HIL OVERALL PASS
```

The seven target cases cover boot-to-READY/Dashboard routing, capture-route
entry, full lifecycle and delayed dashboard return, timer re-entry race,
repeated turns, status/transcript queue pressure, and error recovery. The
unattended suite deliberately injects copied public audio statuses; it does
not independently prove LCD pixels, microphone/speaker acoustics, GPIO38
electrical behavior, or server-originated semantic text. Those physical
acceptance observations are the operator-confirmed portion of this closure.

## Evidence boundary

At Phase-15 closure:

```text
Implementation             COMPLETE
Static review              COMPLETE
Production composition     COMPLETE
idf.py build               PASS (ESP-IDF 6.0.1; 2026-09-06)
Automated lifecycle target PASS (7/7; `fc5a3fa`)
Hardware/manual acceptance CONFIRMED BY USER (`ENDPHASE 15`)
```

## Closure

**Phase 15 = COMPLETE / Static Review Complete / Build PASS / HIL Accepted.**

The HIL plan remains a regression checklist. Do not automatically start a new
phase from this closure.
