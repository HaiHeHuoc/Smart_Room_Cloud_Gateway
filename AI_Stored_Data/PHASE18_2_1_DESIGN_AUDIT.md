# Phase 18.2.1 - Playback Control and PTT Suspension Audit

**STATUS: DESIGN APPROVED / READY FOR PROMPT 2 IMPLEMENTATION**

Audit/design only. No production feature, build verification, or HIL acceptance
is claimed. The locked scope in `PHASE18_2_PLAN.md` is unchanged.

Audited branch: `main_including_Firebase_security` at
`f2597fd8fc60be54a66dc75dc6a7f718b9901c88`.

## 1. Current implementation findings

### Audio manager

**FACT FROM SOURCE**

- `audio_manager` is sole I2S/DMA/source owner. Public starts are retained
  recording, bounded WAV, and generation-keyed PCM16 stream playback.
- `audio_manager_stop_playback()` only sets cooperative cancellation for WAV,
  retained recording, or PCM stream. Each path stops TX, tears down the source,
  and returns to `IDLE`; it is not a pause.
- `audio_manager_status_t` exposes `IDLE`, `PLAYBACK`, and `ERROR`, but has no
  source identity, request ID, pause reason, or playback position.
- Recorded playback keeps `sample_index` only as a local loop variable. WAV
  keeps a copied path only inside its private prefetch object; consumed blocks
  are released. PCM16 TTS is a bounded live ring, not a seekable source.
- WAV prefetch already performs fresh-file reopen/seek after an SD fault. This
  proves byte-offset restart is feasible in the owner, but is not a current
  user pause/resume API.
- WAV reader owns `FILE *` and the SD lease. The manager joins/destroys it
  before source release; only the manager enables TX and writes I2S.

### Playback arbitration

**FACT FROM SOURCE**

- The playback arbiter retains one current and one pending WAV/PCM16 request;
  it supports reject, queue, cooperative cancel, and priority preemption.
- It has no `PAUSED` request state. Direct retained-recording playback is not
  currently represented by this arbiter.
- A cancelled/preempted Xiaozhi stream aborts its bounded ring and has a
  distinct terminal state. The arbiter cannot safely resume it.

### GPIO38, PTT, and TTS

**FACT FROM SOURCE**

- `voice_assistant_ptt_gpio` debounce-delivers GPIO38 edges to the bounded PTT
  queue and retries undelivered stable edges. `voice_assistant_ptt` preserves a
  still-held press while session connection/recovery completes; FIFO release
  prevents a fast tap from later authorizing capture.
- A press is now rejected when `voice_assistant_downlink_is_busy()` is true.
  Busy includes response wait, collection, finalization/PCM drain, and old TTS
  playback. The existing retained press therefore does not yet support local
  playback suspension followed by capture authorization.
- Xiaozhi audio is copied WebSocket event -> bounded Opus queue -> decoder ->
  bounded PCM16 ring -> playback arbiter -> manager I2S/TX. `TTS_STOP` starts
  local drain; busy remains true until terminal stream handling completes.

## 2. Current blockers/gaps

**FACT FROM SOURCE**

1. There is no public pause/resume or retained resume context.
2. Existing cancel is destructive for every source.
3. Neither the arbiter nor PTT represents suspended local audio.
4. PTT has no suspend-then-authorize state or post-turn auto-resume event.
5. Pausing Xiaozhi TX alone would fill its finite ring while server packets
   continue, conflicting with the bounded-stream contract.

## 3. Recommended architecture

**PROPOSAL**

```text
MCP tool / PTT policy
    -> voice_assistant product policy + audio bridge
    -> public audio_manager control API
    -> playback arbiter for arbiter-owned requests
    -> manager-owned source/TX transition
```

- `audio_manager` owns physical state, source identity, committed position,
  fresh reopen/seek, source cleanup, and the actual `PLAYING`/`PAUSED` truth.
- The playback arbiter retains suspended request metadata for requests it owns;
  it must not promote a pending owner during a temporary suspension.
- `voice_assistant` owns PTT-generation correlation and auto-resume eligibility.
  Its PTT submodule owns only press/release policy, never I2S, DMA, `FILE *`,
  SD mount, or reader details.
- `smart_room_mcp_adapter` supplies bounded providers. `xiaozhi_foundation`
  stays the direct MCP/session lifecycle boundary.

**OPEN DECISION:** include retained recorded playback in 18.2.1 or ship WAV
only first. Recommendation: WAV plus retained recording, but only with explicit
arbiter migration/ownership work; do not hide a second owner behind the direct
recorded-playback API.

## 4. Proposed playback state machine

**PROPOSAL**

```text
IDLE -> STARTING -> PLAYING -> PAUSING -> PAUSED_USER | PAUSED_PTT
                                                |              |
                                                +-> RESUMING -> PLAYING
PLAYING/PAUSED_* -> STOPPING -> IDLE
any source failure -> ERROR -> IDLE
```

- Pause is valid only for a resumable local source and completes only when TX
  is inactive and the context is committed.
- Resume is valid only for a matching, non-stale paused generation; request
  acceptance is distinct from observed `PLAYING`.
- Stop is idempotent in `IDLE`; from either paused state it invalidates context.
- Restart is valid only while a retained local source context exists and starts
  from position zero.
- Natural EOF/nonrecoverable errors clear the context. `PAUSED_PTT` permits one
  guarded auto-resume; `PAUSED_USER` never does. Any explicit MCP action wins.

## 5. Source-specific resume policy

| Source | Resume policy | Position owner | Paused resource policy |
|---|---|---|---|
| WAV | **PROPOSAL: yes** | manager-owned copied canonical path + committed data-byte offset + generation | stop TX; join reader; close `FILE *`; release SD lease/prefetch PSRAM; fresh open/identity-check/seek on resume |
| Retained recording | **PROPOSAL: yes** | manager-owned sample index + recording generation/count | stop TX; retain existing PCM24/DSP allocation; no SD lease/reader |
| Xiaozhi PCM/TTS | **PROPOSAL: no** | generation is diagnostics only | cancel/abort ring and close old response channel; never retain old TTS backlog |
| Notification/alarm | **OPEN DECISION** | out of user control unless approved | recommendation: notifications out of scope; alarms remain non-pausable |

## 6. Retained playback context

**PROPOSAL** - private to `audio_manager`, bounded and generation-checked:

```c
source_kind;          /* WAV or retained recording */
source_generation;    /* invalidates stale PTT/MCP completion */
position;             /* WAV data byte or recorded sample index */
source_identity;      /* copied canonical path or retained-buffer generation */
pause_reason;         /* USER or PTT */
```

PTT retains only its PTT generation, source generation, and
`auto_resume_allowed`. No raw `FILE *`, I2S/DMA handle, PCM pointer, reader
task handle, unbounded string, or model-supplied filesystem path crosses a
component boundary.

## 7. PTT retained-press policy

**PROPOSAL**

```text
GPIO38 PRESS while resumable local playback is PLAYING
 -> request PAUSED_PTT
 -> bounded wait for owner acknowledgement
 -> authorize the same still-held press through normal capture arbitration
 -> RELEASE before acknowledgement: revoke capture; do not start microphone

voice turn terminal
 -> valid matching PAUSED_PTT context + no explicit audio action
 -> request resume
 -> otherwise retain explicit state
```

Old Xiaozhi TTS is not local resumable playback: downlink must cancel/close it
through its existing bounded terminal path before a new PTT turn. The PTT task
must receive a terminal downlink/voice event rather than infer completion from
`TTS_STOP`.

## 8. MCP contract proposal

**PROPOSAL - prefer one action tool plus one read-only tool.**

```text
audio.control_playback
  input:  { action: pause | resume | stop | restart }
  output: { accepted, action, state, source, request_generation,
            auto_resume, outcome }

audio.get_playback_state
  input:  {}
  output: copied state, source, request_generation, auto_resume, last_error
```

`outcome` is one of `accepted`, `already_in_state`, `not_resumable`, `busy`,
`invalid_state`, or `failed`. Results must never report physically completed
until the owner snapshot proves it. No arbitrary path, seek, volume, codec,
buffer, I2S, or SD input is exposed.

## 9. Race/concurrency matrix

| Case | Expected result and owner | Failure/retry |
|---|---|---|
| PLAYING + GPIO38 press | PTT requests `PAUSED_PTT`; manager commits then capture arbitrates | timeout/error: no capture; new press retries |
| fast press/release | queued release revokes pending capture | safe; policy decision required for immediate resume |
| pause + natural EOF | generation check yields exactly one terminal state | no stale resume |
| stop while PTT paused | invalidate context/auto-resume | idempotent stop |
| resume while PLAYING | `already_in_state`, no extra reader/TX | query/retry safe |
| restart while paused | position zero then fresh start | owner error clears context as specified |
| repeated pause/stop | idempotent result, one cleanup | safe retry |
| PTT during pause cleanup | arming only; never capture yet | release revokes it |
| Xiaozhi TTS transition | cancel old live stream, never pause it | bounded terminal cleanup first |
| SD error on WAV resume | fresh open/identity/seek + bounded recovery | failure clears context |
| prefetch active on pause | join reader before PAUSED visible | no resume before join |
| network loss in voice turn | downlink abort owns old TTS | auto-resume only after terminal event |
| manager ERROR while paused | invalidate context | explicit later playback only |

## 10. Resource impact and measurement

**PROPOSAL**

No new owner task or full audio buffer. Expect only small copied context/state
and bounded command/event data; measure rather than invent byte counts. Paused
WAV must release reader/lease/prefetch PSRAM; retained recording keeps existing
PCM24; live TTS is aborted. Measure Internal/DMA/PSRAM free-min-largest, manager
/PTT/voice/reader stack HWM, CPU, pause/resume latency, and repeated-turn
resource trend on target.

## 11. Expected Prompt 2/3 files

**PROPOSAL - Prompt 2, owner capability/tests**

- `components/audio/audio_manager/include/audio_manager.h`
- `components/audio/audio_manager/audio_manager.c`
- `components/audio/audio_manager/include/audio_manager_playback_arbiter.h`
- `components/audio/audio_manager/modules/arbitration/src/audio_manager_playback_arbiter.c`
- `components/audio/audio_manager/modules/wav/*` only if a reviewed fresh-start-
  at-offset API is necessary
- new focused test location approved after the prior host suites were retired.

**PROPOSAL - Prompt 3, product policy/MCP integration**

- `components/application/voice_assistant/include/voice_assistant_ptt.h`
- `components/application/voice_assistant/modules/ptt/src/voice_assistant_ptt.c`
- `components/application/voice_assistant/modules/ptt/src/voice_assistant_ptt_gpio.c`
- `components/application/voice_assistant/modules/audio/src/voice_assistant_audio_arbitration_bridge.c`
- `components/application/voice_assistant/modules/downlink/src/voice_assistant_downlink.c`
- `components/application/xiaozhi_foundation/include/xiaozhi_foundation.h`
- new bounded audio MCP modules under `xiaozhi_foundation/modules/`
- `components/application/smart_room_mcp_adapter/` provider/CMake wiring.

These file names are an expected change set. They do not authorize Prompt 3,
Phase 18.2.2, Phase 18.3, or Phase 18.4.

## 12. Risks, acceptance criteria, and approval decisions

**FACT FROM SOURCE:** cancellation is cooperative and cannot preempt an active
synchronous SD read or I2S write. PCM streaming is bounded. Therefore pause
must have finite acknowledgement and must not free reader/ring resources before
their owner has stopped.

**PROPOSAL - acceptance:** copied state must be truthful; WAV/approved recorded
sources resume from their committed position; fast release never starts capture;
explicit audio action suppresses auto-resume; old TTS is never resumed; repeated
races release SD/I2S/queue resources; build, focused tests, and HIL are recorded
separately before closing 18.2.1.

**DECISIONS APPROVED BY HAI - 2026-09-13**

1. Phase 18.2.1 covers local WAV and retained recorded playback.
2. If GPIO38 is released before capture authorization, a valid `PAUSED_PTT`
   local source resumes immediately; microphone capture does not start.
3. Resume position is the committed 256-frame block boundary, not sample-perfect.
   This tolerance must be documented in public result/status wording.
4. The MCP surface is exactly `audio.control_playback` with the bounded actions
   `pause`, `resume`, `stop`, and `restart`, plus read-only
   `audio.get_playback_state`. A command result distinguishes accepted from
   physically observed state.
5. Notifications are out of scope; alarm policy remains non-pausable and is not
   changed by Phase 18.2.1.

## Recommendation

**READY FOR PROMPT 2: YES.**

Prompt 2 may implement only the approved owner-controlled WAV/retained-recording
state/context, 256-frame-boundary resume, and playback-arbitration semantics.
It must not implement MCP registration, GPIO/PTT behavior, Prompt 3 product
policy, Phase 18.2.2, Phase 18.3, or Phase 18.4 without a separate request.

## Prompt 2 implementation resolution

Implemented on `phase/18.2.1-playback-control-core` without changing the
approved architecture:

- the public copied state is `IDLE`, `STARTING`, `PLAYING`, `PAUSING`,
  `PAUSED`, `RESUMING`, `STOPPING`, or transient owner `ERROR`;
- pause reason remains owner metadata (`USER` or `PTT`), while PTT generation,
  GPIO behavior, and automatic-resume policy remain outside `audio_manager`;
- position is exposed as committed mono frames with a 256-frame granularity;
- paused WAV releases TX, reader, `FILE *`, SD lease, queues, and prefetch
  PSRAM, then resumes through fresh open, metadata verification, and seek;
- retained recording resumes from a committed sample/frame index without
  copying its existing PCM24 buffer;
- live PCM16/Xiaozhi pause, resume, and restart are explicitly unsupported;
  cooperative stop remains supported;
- the playback arbiter retains its current WAV request as `PAUSED`, so a
  pending request cannot be promoted during suspension.

This resolves Prompt 2 owner-capability details only. Target audio continuity,
SD timing, repeated-cycle resource behavior, Prompt 3 MCP/PTT integration, and
Phase 18.2.1 completion remain unclaimed.
