# Current Working Checkpoint

Purpose: compact handoff for Sprint 24 AFE/WakeNet runtime stabilization.
Repository source, build output, and target evidence remain authoritative.

Updated: 2026-09-27

## Branch and integration state

- Active branch: `fix/24-afe-runtime-stability`
- Base HEAD: `0e524e1c8fcd7eeaabe755d4d6098d258feded78`
  (`docs(ai): checkpoint PTT uplink robustness fix`). Sprint 24 work remains
  uncommitted.
- No commit, merge, rebase, push, or pull request was performed.

## Observed product failure and evidence

- Earlier target logs alternated between AFE feed-ring full and AFE empty while
  the largest Internal-RAM block was about 7.5 KiB. That is insufficient for
  ESP-SR's documented 8 KiB `afe_mase` worker stack and remains an admission
  risk.
- Xiaozhi's WebSocket wrapper deliberately requests a 12 KiB Internal stack.
  A 7.5 KiB largest block cannot create it, matching
  `websocket_client: Error create websocket task`.
- Latest HIL reached a 31 KiB Internal largest block before AFE, built the AFE
  pipeline, and connected the production WebSocket successfully. It still
  produced AFE FEED-full spam after passive wake listening began. The fetch
  worker could clear `afe_input_ready` after a fetch failure but then wait for
  a later successful feed; a full ring cannot satisfy that condition. This is
  a producer/consumer recovery deadlock, not a harmless ESP-SR warning.
- Target HIL after the recovery fix is still required before acceptance.

## Implemented stabilization

- `wake_word_manager` validates ESP-SR geometry at runtime: PCM16, 16 kHz,
  mono (`"M"` AFE), exact feed/fetch chunks, one feed/fetch channel, no AEC,
  no NS/AGC/SE, and only stock `wn9_hiesp` WakeNet + VAD.
- One `audio_manager` I2S RX owner copies 256-sample / 16-ms frames into one
  fixed local-monitor queue (4 frames, 64 ms, 2,112 B PSRAM). Its zero-wait
  producer cannot block PTT or invoke ESP-SR/network/UI work.
- Feed and fetch are independent workers. Fetch waits for an initial input
  notification, continuously drains ESP-SR while active, and resets only on
  its own side of the generation fence. A fetch failure now performs that
  generation reset instead of waiting indefinitely behind an already-full
  ring; a non-negative full-ring feed also re-notifies the consumer. No
  arbitrary delay or warning suppression was added.
- Feed/fetch workers use 6 KiB PSRAM stacks at priority 5. ESP-SR's private
  `afe_mase` worker is core 1, priority 6, 8 KiB Internal so it outranks the
  bridge workers and drains the two-frame input ring. `audio_manager` remains
  priority 7; `voice_uplink` is priority 6.
- Voice/PTT and its 12 KiB Internal WebSocket task start first. WakeNet is
  admitted afterwards only when a 10 KiB contiguous Internal block exists;
  rejection is explicit and preserves GPIO38 PTT rather than destabilizing the
  mandatory voice path.
- Xiaozhi's dynamic chat-audio task stack is configured for PSRAM. Wake's
  event callback only enqueues a bounded event; a PSRAM `wake_to_voice` task
  invokes the PTT policy. GPIO and WakeNet are independent trigger sources.
- During one accepted WakeNet-to-voice turn only, copied local frames remain
  available for VAD to end the turn. Normal GPIO PTT and speaker playback
  suppress the local monitor. Queue-overflow cancellation can now queue behind
  a pending virtual press, so the bridge fails closed.
- Voice UI status filters shared audio-manager state through Xiaozhi's atomic
  capture/playback reservations. Passive WakeNet capture therefore does not
  present as Xiaozhi `RECORDING` before a PTT turn is actually authorized.

## Bounded diagnostics

- `wake_word_manager_get_status()` reports exact AFE geometry, PCM/feed/fetch
  counters and failures, queue peak/drops, reset drops, max feed/fetch time,
  worker stack high-water marks, and largest Internal block before/after AFE.
- Boot logs report verified AFE geometry and resource-gate rejection without
  per-frame logging.

## Validation actually performed

- `git diff --check`: PASS.
- Wake runtime-policy host test: PASS.
- Audio-manager host tests, including PCM distribution: PASS.
- Voice-assistant host tests (playback, interruption, response epoch, and
  transport fence): PASS.
- ESP-IDF 6.0.1 serialized build, Wake enabled: PASS. Firmware `0x2f22c0`,
  smallest app partition free `0x10dd40` (26%); ESP-SR packs only `wn9_hiesp`
  (284.16 KiB). Windows requires `PYTHONIOENCODING=utf-8` for the model packer.
- Isolated Wake-disabled build: PASS. Firmware `0x290cd0`, 36% app partition
  free; `CONFIG_WAKE_WORD_ENABLE` and `CONFIG_SR_WN_WN9_HIESP` are unset.

## HIL required before Sprint 24 acceptance

1. Capture Internal/DMA/PSRAM free, minima, largest block, task count, and
   stack high-water before Wake, after Wake, and during a Xiaozhi turn.
2. Leave idle Wake listening for several minutes: no AFE full/empty spam, no
   `AFE_FETCH_RECOVERY` loop, no nominal local-monitor drops, and no false
   Xiaozhi `RECORDING` status before an authorized PTT turn.
3. Repeat `Hi ESP`, then run Wake -> speech -> Xiaozhi -> TTS -> rearm.
4. Verify GPIO38 PTT remains independent during/after Wake turns.
5. Repeat many turns and confirm no task/memory leak, transport startup error,
   uplink drop, stale generation, or false early VAD completion.

## Next action

Run the focused Sprint 24 HIL stability checklist before advancing to Prompt 6
or any advanced conversation work.
