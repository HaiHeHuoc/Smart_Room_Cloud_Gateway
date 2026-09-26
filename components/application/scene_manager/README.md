# Scene Manager

`scene_manager` is a small application orchestrator for the four fixed
product scenes, in stable order: `focus`, `relax`, `night`, and `all_off`.
It owns neither hardware nor persistent state.

```text
Local Web -> scene_manager -> light_manager
                         -> voice_assistant playback facade -> audio_manager
```

`scene_manager_apply()` serializes one human-scale request with a mutex and no
queue/task/heap allocation. It applies Light first, then the audio step. It
copies a historical result (`generation`, requested/completed scene, overall,
Light, and Audio outcomes); that result is not a claim that the device still
matches a scene after another frontend changes a manager.

`focus`, `relax`, and `night` apply a complete logical light state and runtime
volume only. They never choose or start content. `all_off` copies the current
light state, changes only `power_on=false`, and asks the established
voice-assistant facade to stop an eligible local source. Live PCM/Xiaozhi and
PTT-owned audio are skipped rather than forcibly interrupted.

The manager does not roll back a successful Light step when Audio is busy,
unavailable, skipped, or fails. Such mixed results are reported as `partial`.
