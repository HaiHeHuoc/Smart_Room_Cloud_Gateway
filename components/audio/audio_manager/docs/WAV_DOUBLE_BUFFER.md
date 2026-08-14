# WAV Ping-Pong Prefetch — Phase 11.4.4

## Goal

Prevent nominal SD/FATFS latency from directly starving I2S playback while
keeping the proven `audio_manager` TX path unchanged. WAV output uses a
stateless full-scale PCM16 map to the shared output ceiling; it does not need a
whole-file peak scan.

The production path is deliberately split into three ownership layers:

```text
SD / FATFS
    |
    v
audio_wav
  - one FILE
  - one sd_card_manager VFS lease
  - synchronous raw reads capped at 4 KiB
    |
    v
audio_wav_prefetch
  - one lower-priority reader task
  - two PSRAM READY/FREE slots
  - fresh-file retry + remount fallback + seek policy
    |
    v
audio_manager
  - sole I2S owner
  - fixed PCM16 full-scale map -> stereo staging
  - existing TX lifecycle/diagnostics
    |
    v
MAX98357A
```

`audio_wav` must not create a FreeRTOS task or allocate the large PSRAM cache.
Keeping the raw reader synchronous avoids nested prefetch ownership and lets the
separate prefetch worker recover by closing/reopening one bounded stream.

## Ping-Pong Policy

`CONFIG_AUDIO_MANAGER_WAV_PREFETCH_SECONDS` controls each logical PSRAM slot.
The default is 10 seconds. For canonical PCM16 mono 16-kHz input:

```text
32,000 bytes/s x 10 s = 320,000 bytes/slot
2 slots                         = 640,000 bytes (~625 KiB PSRAM)
```

The producer fills whichever slot is FREE as soon as possible. It does not wait
for a fixed 70-percent playback threshold. While the manager consumes slot A,
the reader can fill slot B; releasing A returns it to the FREE queue for the
next fill.

The low-level stream still reads at most 4 KiB per `fread()`. Each successful
raw chunk is copied into the current PSRAM slot until that slot is full or the
WAV reaches its final partial block. There is no pre-playback scan or rewind:
once the first slot is READY, `audio_manager` maps each PCM16 sample with
`round(sample * 9000 / 32768)`, then applies user volume and writes I2S.

## Ownership Invariants

- `sd_card_manager` owns mount/unmount/recovery and rejects new VFS leases while
  recovering.
- `audio_wav` owns exactly one SD lease, FILE, and 4 KiB Internal-RAM read
  buffer for one open low-level stream.
- `audio_wav_prefetch` owns the low-level stream, two PSRAM slots, queues,
  worker task, and reopen/seek recovery state.
- `audio_manager` owns I2S TX, playback state, and the static DMA TX staging
  block. It also owns the fixed WAV full-scale-to-ceiling PCM16 map.
- The prefetch worker writes only a FREE slot.
- The manager reads only a READY slot and returns it to the FREE queue after the
  complete block has been consumed.
- No caller closes a FILE, frees PSRAM, or deletes the worker while the reader
  can still access those resources.

## SD Recovery

The recovery policy deliberately distinguishes a single streaming read failure
from a confirmed card/VFS failure.

On the first `fread()` failure while the SD manager is still READY:

1. `audio_wav` logs the read failure, closes the stale FILE, and releases its
   lease without immediately forcing a card unmount/remount.
2. `audio_wav_prefetch` performs one logical recovery attempt by opening a fresh
   FILE on the still-mounted VFS.
3. It verifies the WAV metadata is unchanged, seeks to the last successfully
   committed data offset, and resumes filling the current PSRAM slot.

If that fresh open or seek confirms a real media/VFS failure:

1. The low-level open/seek path reports the error to `sd_card_manager`.
2. `sd_card_manager` enters RECOVERING, drains leases, and performs its existing
   bounded unmount/remount recovery.
3. The same prefetch recovery attempt waits for READY again within its 5-second
   timeout, reopens a fresh FILE, revalidates metadata, seeks to the committed
   offset, and resumes.
4. The playback operation still permits only one logical prefetch recovery
   attempt; repeated failures terminate the operation rather than looping
   indefinitely.

A missing file or malformed/unsupported WAV is not treated as physical SD media
failure.

This ordering is intentional: a transient stream error should not reset a card
that is otherwise still mounted, while a confirmed media failure still reaches
the existing `sd_card_manager` recovery path.

## Cancellation And Cleanup

`audio_manager_stop_playback()` remains cooperative. The manager checks cancel
between existing 256-frame TX writes. The prefetch worker checks its own stop
request between bounded raw reads and while waiting for a FREE slot.

Cleanup uses a join/acknowledgement handshake:

```text
manager requests reader STOP
        |
reader closes audio_wav stream / releases SD lease
        |
reader publishes terminal state + STOPPED
        |
manager acknowledges worker may leave its last EventGroup access
        |
reader notifies owner and deletes itself
        |
manager frees queues/events/PSRAM
```

A pathological VFS transaction that never returns is still a known limitation:
resources are retained rather than force-freed while the worker may be inside
`fread()`. Target-hardware fault testing remains required.

## Diagnostics

The final `WAV_DIAG` line from `audio_manager` aggregates both playback and
prefetch metrics, including:

```text
fixed_gain_q16 / output_peak
read_bytes / streamed_bytes
raw_reads / raw_read_fail / max_raw_read_us
prefetch_block
prefetch_fills / prefetch_fill_fail / max_prefetch_fill_us
sd_resume_offset / sd_resume_attempt / sd_resume_ok / sd_resume_wait
initial_wait / boundary_wait / prefetch_starve
reader_hwm
tx_requested / tx_written / tx_q_ovf / tx_timeout / tx_partial
```

For nominal playback, the strongest software-side prefetch signal is:

```text
prefetch_starve = 0
```

A non-zero value means the manager reached a ping-pong boundary before another
READY slot arrived within the bounded boundary wait. Correlate that event with
audible output and TX diagnostics on hardware.

When fault testing, distinguish the two recovery outcomes:

```text
Fresh-file fast retry:
  sd_resume_attempt = 1
  sd_resume_ok      = 1
  SD may remain READY throughout

Confirmed media failure:
  SD transitions RECOVERING
  remount occurs
  the same logical retry reopens/seeks after READY
```

## Phase 11.4.4 Hardware Acceptance

Hardware is still required before calling the path glitch-free or stable.
Validate at least canonical 5 s, 10 s, 11 s, 30 s, and 60 s WAV files with the
full Gateway services active.

Expected nominal results:

```text
WAV playback completes to EOF
fixed_gain_q16 = 18000
output_peak <= 9000
read_bytes == streamed_bytes == WAV data bytes
prefetch_starve = 0
tx_timeout = 0
tx_partial = 0
tx_q_ovf = 0
no audible gap at ping-pong boundaries
no WDT/crash
PLAYBACK -> IDLE
speaker DIN returns LOW
```

Also validate cancellation near a buffer boundary, manager stop/restart, SD
removal/recovery, and the golden microphone regression:

```text
INMP441 record -> DSP -> recorded playback
```

The WAV prefetch/recovery work does not modify microphone DSP algorithms, I2S
format, DMA geometry, startup discard, or slot detection. The intentional shared
`AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16` define changes the common recorded/WAV
output ceiling to 9000. WAV receives a fixed scalar, not per-file loudness
normalization or a dynamic limiter.
