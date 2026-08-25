# Phase 15 Voice Assistant UI / Conversation Presentation Progress

Updated: 2026-08-25
Branch: `phase/15-voice-assistant-ui`
Current checkpoint: **15-D — Production ASSISTANT Text Wiring**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 15 follows the same review cadence as Phases 13 and 14. Implement one checkpoint, stop, explain what changed, and continue only when Hải says `tiếp tục`.

Planned checkpoints:

1. 15-A — production voice event/UI model. ✅
2. 15-B — dedicated Voice Assistant screen and lifecycle presentation. ✅
3. 15-C — production USER transcript wiring. ✅
4. 15-D — production ASSISTANT text wiring. ✅
5. 15-E — repeated-turn/history/error UX + presentation refinement. NEXT
6. 15-F — FINAL review/composition/docs/deferred HIL.

At 15-F explicitly notify Hải that it is the final Phase-15 prompt before software closure.

## 15-A / 15-B summary

Phase 15 now has a production `voice_assistant_ui_model` and a GUI adapter. Presentation flows only through copied data:

```text
voice_assistant / semantic Xiaozhi events
-> voice_assistant_ui_model
-> voice_assistant_ui_gui_adapter
-> app_gui latest-value queue
-> app_gui UI task
-> LVGL
```

The production model is separate from the temporary Phase-12 validation UI model. The existing UI-task-owned Xiaozhi screen is reused/promoted as the visual surface; no voice/Xiaozhi callback calls LVGL directly.

Current visual mapping limitation remains explicit: production CONNECTING/THINKING/RECOVERING still render through the legacy PROCESSING visual state until 15-E refinement.

## 15-C / 15-D semantic text implementation

The project lock pins `espressif/esp_xiaozhi` 0.1.2. Production semantic text is observed through the Xiaozhi CHAT_TEXT callback contract and promoted through a dedicated project-owned text observer that is separate from both session-status and binary/TTS response callbacks.

Foundation semantic roles:

```text
XIAOZHI_FOUNDATION_TEXT_ROLE_USER
XIAOZHI_FOUNDATION_TEXT_ROLE_ASSISTANT
```

The upstream text pointer is callback-lifetime only. `xiaozhi_foundation` does not retain it; the UI model copies it synchronously into its bounded 192-byte project-owned buffer before callback return.

### USER path — 15-C

```text
Xiaozhi CHAT_TEXT / USER
-> xiaozhi_foundation semantic text bridge
-> current session generation
-> voice_assistant_ui_model_post_user_text()
-> copied user_text
-> GUI adapter
-> app_gui UI task
-> LCD User text
```

### ASSISTANT path — 15-D

15-D now consumes the already-promoted ASSISTANT role:

```text
Xiaozhi CHAT_TEXT / ASSISTANT
-> xiaozhi_foundation semantic text bridge
-> current session generation
-> voice_assistant_ui_model_post_assistant_text()
-> copied assistant_text
-> GUI adapter
-> app_gui UI task
-> LCD Assistant text
```

USER and ASSISTANT use the same generation, lifetime, bounds and truncation rules. A stale generation is rejected by the presentation model.

Assistant text presentation is intentionally independent from response audio playback. Receiving assistant text does not wait for, start, stop or directly control speaker playback. Audio remains on the Phase-14 response/downlink/audio-manager path.

Therefore the normal order may be:

```text
assistant semantic text arrives
-> LCD text updates

response audio arrives later/concurrently
-> downlink path
-> audio_manager playback
-> speaker
```

No coupling requires the LCD to wait for TTS playback completion.

## Semantic text callback ownership

The project keeps three distinct observer paths:

```text
xiaozhi session status
    -> voice_assistant

xiaozhi binary/TTS response
    -> voice_assistant_downlink

xiaozhi semantic USER/ASSISTANT text
    -> voice_assistant_ui_model
```

This prevents UI text from stealing or overloading the response callback used by Phase-14 speaker playback.

## Source-local Xiaozhi bridge

`xiaozhi_session.c` has its production `esp_xiaozhi_chat_init()` source-locally redirected to `xiaozhi_foundation_chat_init_bridge()`.

The bridge preserves and forwards the original protocol callback and additionally publishes CHAT_TEXT semantic events. This is a controlled integration seam intended to avoid rewriting the large session lifecycle. A real ESP-IDF build is required before claiming compatibility with the pinned component/toolchain; refactor the seam later if build/maintenance evidence shows it is fragile.

## Static review notes

1. USER and ASSISTANT text are copied before the upstream callback returns.
2. semantic callback performs no LVGL, I2S, storage or network lifecycle operation.
3. app_gui receives only copied bounded fields.
4. new non-zero session generation clears old conversation text.
5. stale-generation semantic text cannot overwrite a newer session presentation.
6. USER and ASSISTANT text are independently valid/truncated.
7. assistant text is not coupled to audio playback completion.
8. existing Phase-13/14 response and session callbacks remain separate.
9. no ESP-IDF build or HIL PASS is claimed.

## Not implemented yet

- no multi-turn history buffer/list;
- no exact production visual enum for CONNECTING vs THINKING vs RECOVERING;
- no explicit per-turn presentation ID beyond the long-lived session generation;
- no waveform/audio-level visualization;
- no GUI PTT button;
- no new hardware behavior.

## Next checkpoint — only after user says `tiếp tục`

**15-E — Repeated-Turn / History / Error UX + Presentation Refinement**

Planned scope:

1. define bounded multi-turn presentation policy without unbounded transcript growth;
2. decide when previous USER/ASSISTANT text is retained or replaced across PTT turns in the same long-lived session;
3. refine visual distinction for CONNECTING / THINKING / RECOVERING if practical without destabilizing `app_gui`;
4. improve ERROR/recovery presentation and stale-text cleanup rules;
5. keep GUI PTT/waveform optional and out of scope unless needed for the Phase-15 acceptance contract.
