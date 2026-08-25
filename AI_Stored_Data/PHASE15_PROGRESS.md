# Phase 15 Voice Assistant UI / Conversation Presentation Progress

Updated: 2026-08-25
Branch: `phase/15-voice-assistant-ui`
Current checkpoint: **15-B — Voice Assistant Screen / Lifecycle Presentation**
Status: **IMPLEMENTED WITH DOCUMENTED VISUAL-MAPPING LIMITATION / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 15 follows the same review cadence as Phases 13 and 14. Implement one checkpoint, stop, explain what changed, and continue only when Hải says `tiếp tục`.

Planned checkpoints:

1. 15-A — production voice event/UI model. ✅
2. 15-B — dedicated Voice Assistant screen and lifecycle presentation. ✅
3. 15-C — production USER transcript wiring. NEXT
4. 15-D — production ASSISTANT text wiring.
5. 15-E — repeated-turn/history/error UX + presentation refinement.
6. 15-F — FINAL review/composition/docs/deferred HIL.

At 15-F explicitly notify Hải that it is the final Phase-15 prompt before software closure.

## 15-A summary

Added the production `voice_assistant_ui_model` above `voice_assistant` with:

- copied lifecycle state;
- session generation;
- last error;
- bounded 192-byte user/assistant text buffers;
- validity/truncation flags;
- stale-generation rejection;
- stale text clearing on a new non-zero session generation;
- copied observer/getter;
- no LVGL calls.

The production model is intentionally separate from the temporary Phase-12 `xiaozhi_foundation_ui_status_t` validation model.

## 15-B — implemented

### Production GUI adapter

Added:

- `include/voice_assistant_ui_gui_adapter.h`;
- `voice_assistant_ui_gui_adapter.c`;
- `app_gui` as a `voice_assistant` component dependency;
- production composition startup for the GUI adapter after the UI model starts.

Flow:

```text
voice_assistant
-> voice_assistant_ui_model
-> voice_assistant_ui_gui_adapter
-> app_gui_post_xiaozhi_status()
-> app_gui latest-value queue
-> app_gui UI task
-> LVGL Xiaozhi/Voice visual surface
```

The adapter never calls LVGL. It only copies presentation data and posts an app_gui screen request.

### Screen routing

The production adapter requests the existing Xiaozhi visual surface when the production model enters an active/relevant state:

```text
CONNECTING
READY
LISTENING
THINKING
SPEAKING
RECOVERING
ERROR
```

`IDLE` does not force a screen transition. If the Voice/Xiaozhi screen is already active, model updates continue to render through its queue.

This preserves existing sensor/Wi-Fi/cloud screens until an actual voice lifecycle requires presentation.

### Visual-surface promotion decision

`app_gui` already contains a Xiaozhi screen with:

- connection/state label;
- state indicator;
- detail/duration labels;
- bounded user text label;
- bounded assistant text label.

15-B reuses/promotes this UI-task-owned visual surface rather than duplicating another LVGL screen. The production backend does **not** reuse the Phase-12 validation model; the GUI adapter is the translation boundary.

### Current lifecycle mapping limitation

The existing `app_gui` Xiaozhi enum was designed for Phase-12 validation and has fewer visual states than the production Phase-15 model. Current mapping is therefore:

```text
Production IDLE        -> DISCONNECTED
Production CONNECTING  -> PROCESSING
Production READY       -> READY
Production LISTENING   -> LISTENING
Production THINKING    -> PROCESSING
Production SPEAKING    -> RESPONDING
Production RECOVERING  -> PROCESSING
Production ERROR       -> ERROR
```

This means the backend model retains exact `CONNECTING / THINKING / RECOVERING`, but the current promoted visual surface renders those three as the legacy `PROCESSING` state. This limitation is explicit and must not be described as exact-state visual acceptance.

15-E/final presentation refinement may promote the app_gui enum/screen to exact production state labels if that remains useful after transcript/assistant text are wired.

### Text placeholders

15-B copies the production model's user/assistant buffers into the existing bounded screen fields. At this checkpoint they normally remain empty because real production semantic text ingress is not wired until 15-C/15-D.

No fake transcript/assistant text is generated.

### Production composition order

```text
audio_manager READY
-> voice_assistant init/start
-> voice_assistant_ui_model init/start
-> voice_assistant_ui_gui_adapter init/start
-> PTT policy
-> uplink
-> downlink
-> PTT GPIO
```

Phase-12 validation mode still suppresses the complete production voice stack, so validation and production presentation do not concurrently own the Xiaozhi UI path under normal branch configuration.

## Static review notes

1. GUI adapter callback performs copied translation and queue/screen requests only.
2. No LVGL API is called outside `app_gui` UI ownership.
3. The model observer remains single-owner: UI model observes `voice_assistant`; GUI adapter observes UI model.
4. No second direct `voice_assistant` status observer was added.
5. app_gui remains independent from voice-assistant internals; dependency direction is `voice_assistant -> app_gui`, avoiding a component cycle.
6. Existing Phase-12 validation UI API remains available.
7. Production active states request the Voice/Xiaozhi screen; IDLE does not steal routing from the normal dashboard.
8. Exact visual distinction for CONNECTING/THINKING/RECOVERING is a known remaining presentation limitation.
9. No ESP-IDF build or target HIL PASS is claimed.

## Not implemented yet

- no real production USER transcript ingress yet;
- no real production ASSISTANT text ingress yet;
- no multi-turn conversation history;
- no exact production visual enum for CONNECTING vs THINKING vs RECOVERING;
- no waveform/audio-level visualization;
- no GUI PTT button;
- no new hardware behavior.

## Next checkpoint — only after user says `tiếp tục`

**15-C — Production USER Transcript Wiring**

Planned scope:

1. inspect/verify the exact pinned `esp_xiaozhi` semantic text event and USER role;
2. promote USER text through `xiaozhi_foundation` as a copied/project-owned production semantic event;
3. preserve callback lifetime and generation filtering;
4. feed `voice_assistant_ui_model_post_user_text()`;
5. verify the app_gui screen receives the copied transcript through the existing 15-B adapter;
6. do not wire ASSISTANT text until 15-D.
