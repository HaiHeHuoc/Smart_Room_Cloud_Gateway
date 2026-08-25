# Phase 15 Voice Assistant UI / Conversation Presentation Progress

Updated: 2026-08-25
Branch: `phase/15-voice-assistant-ui`
Current checkpoint: **15-E — Repeated-Turn / History / Error UX**
Status: **IMPLEMENTED WITH DOCUMENTED LEGACY VISUAL-MAPPING LIMITATION / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 15 follows the same review cadence as Phases 13 and 14. Implement one checkpoint, stop, explain what changed, and continue only when Hải says `tiếp tục`.

Planned checkpoints:

1. 15-A — production voice event/UI model. ✅
2. 15-B — dedicated Voice Assistant screen and lifecycle presentation. ✅
3. 15-C — production USER transcript wiring. ✅
4. 15-D — production ASSISTANT text wiring. ✅
5. 15-E — repeated-turn/history/error UX + presentation refinement. ✅
6. 15-F — FINAL review/composition/docs/deferred HIL. NEXT

At 15-F explicitly notify Hải that it is the final Phase-15 prompt before software closure.

## 15-A through 15-D summary

Production presentation path:

```text
voice_assistant / Xiaozhi semantic text
-> voice_assistant_ui_model
-> voice_assistant_ui_gui_adapter
-> app_gui latest-value queue
-> app_gui UI task
-> LVGL Voice/Xiaozhi visual surface
```

Semantic USER and ASSISTANT text are copied synchronously from callback-lifetime Xiaozhi text into bounded 192-byte project-owned fields. Audio response remains independent on the Phase-14 downlink/audio-manager path.

## 15-E — repeated-turn presentation policy

### Why session generation is insufficient

The production Xiaozhi connection is intentionally long-lived. Several PTT turns can therefore share one `session_generation`. The UI cannot use session generation alone to know that a new user utterance started a new displayed turn.

15-E adds a presentation-only:

```text
turn_sequence
```

This is not a protocol transaction ID and is not used for transport/audio ownership. It is only a bounded UI identity/counter inside the current long-lived session.

### Latest-turn policy

The 160x128 display does not retain an unbounded multi-message history. Phase 15 intentionally uses a **latest-turn view**:

```text
USER semantic text for current session
-> increment turn_sequence (non-zero wrap handling)
-> clear assistant text from previous turn
-> show new USER text

ASSISTANT semantic text
-> retain current USER text
-> replace/update ASSISTANT text for current latest turn
```

Example:

```text
turn 1
User: Turn on the light
Assistant: Done

turn 2 USER arrives
-> turn_sequence 1 -> 2
User: What is the temperature?
Assistant: <cleared until response arrives>

turn 2 ASSISTANT arrives
User: What is the temperature?
Assistant: 27 degrees
```

This prevents an assistant response from the previous displayed turn remaining beside a new user transcript.

### Session transition

When a new non-zero `session_generation` is observed:

```text
clear USER text
clear ASSISTANT text
turn_sequence = 0
```

Stale text from an old session remains rejected by the existing generation check.

### Error / recovery UX policy

ERROR or RECOVERING state changes do **not** erase the current latest-turn text. The UI keeps the most recent conversation context visible while showing the lifecycle error/recovery state. Text is cleared only by an explicit clear or by a new session generation/new USER turn policy above.

This is intentional: an error should not destroy the user's visible conversational context.

## Presentation-state refinement result

The production model still retains exact states:

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

The reused legacy `app_gui` Xiaozhi visual surface still has the older Phase-12 visual enum, so its current translation remains:

```text
CONNECTING  -> PROCESSING
THINKING    -> PROCESSING
RECOVERING  -> PROCESSING
LISTENING   -> LISTENING
SPEAKING    -> RESPONDING
READY       -> READY
ERROR       -> ERROR
```

15-E deliberately does not perform a large rewrite of `app_gui.c` solely to split those three labels before any build/HIL evidence exists. The backend semantic state remains exact; this is a **presentation limitation**, not lost state information.

15-F final review must decide whether this limitation is acceptable for Phase-15 MVP acceptance or whether a small post-build GUI refinement should be scheduled. It must not claim exact CONNECTING/THINKING/RECOVERING LCD labels unless target evidence/code actually supports them.

## Static review notes

1. `turn_sequence` is presentation-only and resets on a new session generation.
2. USER text starts a new latest-turn presentation and clears only prior assistant text.
3. ASSISTANT text does not increment the turn counter.
4. session change still clears all conversation text.
5. ERROR/RECOVERING retain current text for diagnostic/user context.
6. no unbounded conversation-history allocation was introduced.
7. semantic callback still performs no LVGL/I2S/storage/network lifecycle operation.
8. no ESP-IDF build or HIL PASS is claimed.

## Intentionally out of scope

- full scrollable multi-turn conversation history;
- waveform/audio-level visualization;
- GUI PTT button;
- exact legacy `app_gui` enum rewrite before build/HIL evidence;
- new hardware behavior.

## Next checkpoint — only after user says `tiếp tục`

**15-F — FINAL Phase-15 Review / Composition / Documentation / Deferred HIL**

This is the final Phase-15 prompt. Planned scope:

1. review the complete Phase-15 diff and callback/dependency ownership;
2. reconcile production composition and validation isolation;
3. decide/document the exact visual-state limitation for MVP closure;
4. produce Phase-15 HIL acceptance plan including USER/ASSISTANT text and repeated turns;
5. update project state/next-work routing;
6. if no new software blocker is found, close Phase 15 as Software Complete / Build + HIL Pending.
