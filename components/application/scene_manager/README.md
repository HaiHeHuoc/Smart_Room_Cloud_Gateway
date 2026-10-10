# Scene Manager

## Purpose

`scene_manager` orchestrates four fixed product scenes in stable order:
`focus`, `relax`, `night`, and `all_off`. It owns neither hardware, persistent
state, a task, nor a queue.

```text
Local Web -> scene_manager -> light_manager
                         -> voice_assistant playback facade -> audio_manager
```

## Public API And Results

- `scene_manager_init()` creates the manager mutex and initial copied status.
- `scene_manager_get_catalog()` copies the fixed scene catalog.
- `scene_manager_get_status()` copies the latest orchestration result.
- `scene_manager_apply()` accepts one exact built-in ID from normal task
  context and serializes a human-scale request with the mutex.

The status `generation`, requested/completed scene, overall, light, and audio
outcomes are historical results. They are not a claim that the physical device
still matches a scene after another frontend changes an owner.

## Behavior And Ownership

`focus`, `relax`, and `night` apply a complete logical light state and runtime
volume only; they never choose or start content. `all_off` copies current light
state, changes only `power_on=false`, and asks the voice-assistant facade to
stop an eligible local source. Live PCM/Xiaozhi and PTT-owned audio are skipped
rather than forcibly interrupted.

The manager applies light first and then the audio step. It does not roll back
a successful light operation when audio is busy, unavailable, skipped, or
fails; mixed outcomes are reported as `partial`. `light_manager` remains the
logical light/effect owner and `audio_manager` remains the I2S/playback owner.

## Concurrency And Limitations

There is no background worker or heap allocation. A concurrent apply request
returns a copied `busy` outcome. All public APIs are task-context APIs; callers
must not invoke them from ISR/callback paths that cannot take the mutex.

Scenes are intentionally fixed and do not provide arbitrary colors, paths,
playlist selection, persistent scene editing, or a guarantee that every domain
operation completes physically before the synchronous result is returned.
