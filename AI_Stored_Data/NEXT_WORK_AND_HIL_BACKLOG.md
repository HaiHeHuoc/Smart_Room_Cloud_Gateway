# Next Work + Deferred HIL Backlog

Updated: 2026-08-25
Authoritative development branch: `phase/13-voice-assistant`
Purpose: cross-session/Codex routing for the question **"hiện tại nên làm gì tiếp theo?"**

## Routing rule

Whenever Hải asks what to do next, always surface BOTH deferred hardware backlogs below before recommending new coding work.

Do not silently forget Phase 12 or Phase 13 HIL just because hardware is unavailable.

At the same time, hardware-unavailable items must not block software development:

```text
hardware unavailable
-> show Phase 12 HIL backlog
-> show Phase 13 HIL backlog
-> mark both DEFERRED
-> recommend next software-only roadmap task
```

Current next software-only roadmap task after Phase 13 is:

**Sprint 14 — Push-To-Talk Voice MVP**

Do not start Sprint 14 automatically unless Hải requests it.

---

# Deferred HIL — Phase 12

Phase 12 status: **software implementation complete / selected target acceptance pending**.

Primary test branch:

`test/xiaozhi-p2f-known-audio-e2e`

Primary handoffs:

- `AI_Stored_Data/P2F_KNOWN_AUDIO_HIL.md`
- `AI_Stored_Data/PHASE12_HIL_TEST_PLAN.md` on the Phase-12 HIL branch where present
- `AI_Stored_Data/BOOT_STARTING_DEBUG.md`

Hardware backlog:

1. P2-F known-audio E2E from SD:
   - fixture READY;
   - 33/33 Opus frames TX;
   - semantically correct USER STT;
   - non-empty ASSISTANT text;
   - server response audio RX;
   - clean teardown.
2. BOOT `Starting...` regression acceptance.
3. Real AP/Wi-Fi/Internet/DNS/TLS/service-loss behavior.
4. Runtime resource/cleanup baseline under the above real target flows.

Current routing state when hardware is unavailable:

**DEFERRED — DO NOT BLOCK SPRINT 14 SOFTWARE WORK.**

---

# Deferred HIL — Phase 13

Phase 13 status: **Software Complete / Build + Hardware Acceptance Pending**.

Dedicated test branch:

`test/phase13-voice-assistant-hil`

Primary handoff on that branch:

`AI_Stored_Data/PHASE13_HIL_TEST_BRANCH.md`

The test branch contains a test-only app wrapper and HIL supervisor. It preserves production `main.c`, waits for Gateway steady state, then exercises the Phase-13 production `voice_assistant` session surface.

Automatic HIL matrix:

1. Wait for network ONLINE + `audio_manager` IDLE.
2. Initialize/start `voice_assistant` and require IDLE.
3. Exercise 32 rapid copied audio-status posts under latest-value coalescing.
4. Run three production lifecycle cycles by default:

```text
IDLE
-> CONNECTING
-> real Xiaozhi READY
-> explicit end
-> intentional cleanup
-> IDLE
```

5. Verify duplicate begin/end rejection.
6. Capture internal/largest-block/PSRAM resource checkpoints.
7. Require explicit automatic PASS summary.

Manual real-network recovery window:

```text
READY
-> externally remove AP/Internet/service
-> ERROR
-> explicit recover()
-> RECOVERING
-> IDLE
```

If no external fault is injected during the window, this stage must be reported `SKIP`, not falsely marked PASS.

Current routing state when hardware is unavailable:

**DEFERRED — TEST INFRASTRUCTURE READY, TARGET EVIDENCE PENDING.**

---

# What to answer when asked "làm gì tiếp theo?"

## If hardware is NOT available

Answer in this order:

1. Remind that Phase 12 HIL remains deferred on `test/xiaozhi-p2f-known-audio-e2e`.
2. Remind that Phase 13 HIL remains deferred on `test/phase13-voice-assistant-hil`.
3. State that neither should block current vibe coding.
4. Recommend the next software task: **Sprint 14 — Push-To-Talk Voice MVP**.

Example concise state:

```text
Phase 12 HIL  -> DEFERRED / test branch ready
Phase 13 HIL  -> DEFERRED / test branch ready
Phase 13 SW   -> COMPLETE
Next coding   -> Sprint 14 PTT Voice MVP
```

## If hardware IS available

Before declaring Phase 12 or Phase 13 fully accepted, run the corresponding HIL branches and preserve complete serial evidence.

Recommended order unless a specific regression requires otherwise:

1. Phase 12 HIL branch — close older deferred transport/startup/resource evidence first.
2. Phase 13 HIL branch — validate the new production voice-assistant lifecycle/recovery layer.
3. Fix failures on the owning phase branch, propagate forward, and rerun the affected HIL.
4. Only mark a phase hardware-accepted after its documented PASS criteria are met.

## Evidence discipline

Never turn any of these into PASS based only on source review, simulated/private protocol calls, or expected behavior.

Use these states precisely:

- `IMPLEMENTED` — code exists;
- `STATIC REVIEW COMPLETE` — source-level review performed;
- `BUILD VERIFIED` — actual ESP-IDF build evidence exists;
- `HIL PASS` — target board serial/runtime evidence satisfies the documented contract;
- `DEFERRED HIL` — hardware evidence intentionally postponed.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
