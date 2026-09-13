# Phase 18.2 — Audio Controlled Actions Plan

Status: **PLANNING / Q&A IN PROGRESS — SCOPE NUMBERING LOCKED**

Updated: 2026-09-13
Integration branch: `main_including_Firebase_security`

## Purpose

This file is the durable anti-drift plan for Phase 18.2. It records the
numbering and design direction agreed during Q&A before implementation starts.
It does not claim that Phase 18.2 source has been implemented, built, or HIL
validated.

Current Phase-18 execution state remains:

```text
18.1    NeoPixel MCP control                                  COMPLETE
18.2.1  Audio Playback Control + PTT Suspension/Auto-Resume   PLANNING
18.2.2  audio.play_recorded / bounded playback variant        PLANNING
18.3    Existing previously planned scope                     NOT STARTED / UNCHANGED
18.4    Existing previously planned scope                     NOT STARTED / UNCHANGED
```

Do not collapse 18.2.1 and 18.2.2 back into one coarse `18.2` implementation
slice. Do not renumber, replace, or silently repurpose Phase 18.3 or 18.4.

---

# Phase 18.2.1 — Audio Playback Control + PTT Audio Suspension & Auto-Resume

## Goal

Allow bounded control of an already-existing playback operation while adding a
natural GPIO38 Push-To-Talk interruption policy for resumable local audio.

The intended user experience is:

```text
local audio playing
    -> GPIO38 press
    -> temporarily suspend playback and retain resume context
    -> continue the same physical PTT intent into Xiaozhi interaction
    -> if the voice turn does not change audio state, resume after Xiaozhi reply
    -> if the user gives an audio-control command, that command overrides the
       automatic resume policy
```

GPIO38 must not route through MCP just to interrupt local playback. PTT is a
local product-control path. MCP and PTT may reuse the same project-owned audio
control capability, but both must preserve `audio_manager` ownership.

## Playback controls in scope

The playback-control feature set is:

```text
pause
resume
stop
restart
get playback state (read-only companion)
```

Current preferred MCP shape, still subject to final schema review during Q&A:

```text
audio.control_playback
    action = pause | resume | stop | restart

audio.get_playback_state
    read-only copied state
```

Do not create direct MCP access to I2S, DMA, file handles, WAV-reader internals,
SD mount ownership, or arbitrary audio buffers.

## Required playback state/context

Phase 18.2.1 must define enough project-owned state to distinguish at least:

```text
IDLE
PLAYING
PAUSED
```

For resumable sources, retain a bounded playback context sufficient to resume
from the interrupted position without loading the whole source into memory.

Expected examples:

```text
WAV/local file   -> retain safe source identity + committed playback offset
recorded audio   -> retain safe source identity + sample/index position
```

Exact state representation belongs to the owning audio layer and must be frozen
after source/arbitration review. Do not expose raw internal pointers/handles
through application or MCP APIs.

## GPIO38 PTT policy

When resumable local audio is playing:

1. GPIO38 press requests temporary suspension, not destructive stop.
2. The current resumable playback position/context is retained.
3. The same retained physical press continues into PTT after bounded suspension
   is complete; the user must not need to press GPIO38 a second time.
4. If the user releases GPIO38 before PTT authorization completes, the pending
   capture intent is revoked; cleanup completion must not open the microphone
   unexpectedly.
5. The voice interaction proceeds through the normal Xiaozhi path.
6. After the Xiaozhi response reaches its terminal state:
   - if no audio-control action changed the suspended context, resume from the
     retained position;
   - if an audio-control action changed the context, that explicit command wins.

Examples:

```text
song @ 02:37
-> GPIO38
-> temporary pause @ 02:37
-> "Nhiệt độ phòng bao nhiêu?"
-> Xiaozhi answers
-> no audio-control command occurred
-> resume @ 02:37
```

```text
song @ 02:37
-> GPIO38
-> temporary pause
-> "Dừng nhạc đi"
-> audio control = stop
-> discard resume intent/context as defined by the owner
-> Xiaozhi response finishes
-> remain stopped; no automatic resume
```

## Audio-intent override policy

An explicit audio command issued during the voice turn overrides default
auto-resume behavior.

Required semantics:

```text
pause    -> remain paused after the voice response
resume   -> resume the retained source according to safe response/playback
            sequencing; do not overlap ordinary Xiaozhi TTS unnecessarily
stop     -> terminate playback and invalidate automatic-resume intent
restart  -> restart the retained/current source from its defined beginning
```

Exact acknowledgement-versus-physical-completion semantics must be defined
before implementation. Do not claim `stopped`, `resumed`, or `restarted`
physically until the owning audio state can prove it.

## Source policy

Initial direction:

```text
local WAV playback        resumable
retained recorded audio   resumable when source semantics permit
Xiaozhi live TTS stream   interrupt/cancel old response; do not attempt exact
                          resume of the interrupted server TTS response
```

The Xiaozhi downlink is a bounded live stream, not a seekable file. Do not keep
an unbounded TTS backlog merely to resume an old spoken response after a new
PTT turn.

## Out of scope for 18.2.1

Do not add these merely as part of playback control:

```text
arbitrary filesystem paths from MCP
volume control
seek forward/backward
seek-to-timestamp
next/previous track
playlist management
internet audio sources
mute/EQ/speed control
arbitrary codec expansion
```

Starting/selecting a new bounded playback source belongs to Phase 18.2.2, not
18.2.1.

## Architecture constraints

Preserve these ownership rules:

```text
MCP / smart_room_mcp_adapter
    -> public bounded audio control API
    -> audio arbitration / audio_manager owner
    -> source/TX cleanup or suspension
    -> sole I2S/DMA ownership remains in audio_manager
```

PTT follows a local product path but must converge on the same owner-controlled
audio policy. No new component may become a second I2S, DMA, WAV `FILE *`, SD
lease, or live-PCM owner.

The implementation should reuse the current bounded WAV/recorded/PCM paths and
must not add whole-file buffering just to support pause/resume.

## Validation direction

At minimum, later implementation/HIL planning must cover:

```text
pause -> resume at retained position
pause -> stop
pause -> restart
repeated pause/resume
GPIO38 while local playback is active
same-press transition from suspension into PTT
fast GPIO38 release during suspension
non-audio question -> automatic resume
audio command -> override automatic resume
new playback after stop/cancel
no stale SD lease / WAV reader / PCM stream / I2S ownership
no monotonic Internal/DMA/PSRAM loss
no stack regression or playback/voice deadlock
```

Current resource measurements must be rechecked under equivalent integrated
workload after implementation; older snapshots are not automatic acceptance of
new pause/resume concurrency.

---

# Phase 18.2.2 — `audio.play_recorded` / Bounded Playback Variant

## Goal

Add a bounded controlled action that starts an approved audio source through the
existing project-owned audio playback path.

The previously approved Phase-18 direction is preserved:

```text
audio.play_recorded
```

or a bounded allowlisted playback variant if Q&A concludes that it better fits
the product without breaking ownership or safety constraints.

This subphase is distinct from 18.2.1:

```text
18.2.1  controls an existing playback and PTT suspension/resume policy
18.2.2  starts/selects an approved playback source
```

## SD music-folder candidate — not yet frozen

A candidate discussed during Q&A is voice selection of tracks from a bounded
music catalog located under a dedicated SD-card area, for example:

```text
/sdcard/music/
```

Possible future MCP shape under 18.2.2:

```text
audio.list_tracks    read-only
audio.play_track     controlled
```

If adopted, MCP must operate on bounded logical track IDs/names from a
project-owned catalog. It must not accept arbitrary raw filesystem paths from
the model.

Example safe direction:

```text
voice intent
-> bounded track ID/name
-> project-owned allowlisted catalog lookup
-> safe internal SD path
-> existing audio_manager playback path
```

The catalog format, maximum track count, naming/alias policy, matching rules,
source formats, and whether this replaces or supplements `audio.play_recorded`
remain **Q&A decisions, not approved implementation yet**.

## Out of scope unless explicitly approved during 18.2.2 planning

- arbitrary SD filesystem browsing;
- arbitrary model-supplied paths;
- internet streaming services;
- playlist/shuffle/recommendation engines;
- compressed codec expansion solely for feature count;
- transferring SD mount/VFS ownership into MCP or application glue.

---

# Phase 18.3 / 18.4 preservation

Phase 18.3 and Phase 18.4 keep their previously planned numbering and scope.
This Phase-18.2 subdivision does not consume, rename, or repurpose either phase.

Any future roadmap reconciliation must preserve completed Phase 18.1 history
and this 18.2.1/18.2.2 split unless Hải explicitly changes the plan.

## Evidence status

This file records planning decisions only:

```text
18.2.1 implementation   NOT STARTED
18.2.1 build            NOT RUN / NOT CLAIMED
18.2.1 HIL              NOT RUN / NOT CLAIMED
18.2.2 implementation   NOT STARTED
18.2.2 build            NOT RUN / NOT CLAIMED
18.2.2 HIL              NOT RUN / NOT CLAIMED
```
