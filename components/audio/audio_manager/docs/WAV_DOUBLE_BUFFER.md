# WAV Ping-Pong Prefetch — Phase 11.4.4

## Goal

Prevent nominal SD/FATFS latency from directly starving I2S playback while
keeping the proven `audio_manager` TX path unchanged.

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
  - recovery/reopen/seek policy
    |
    v
audio_manager
  - sole I2S owner
  - PCM16 mono -> stereo staging
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
WAV reaches its final partial block.

## Ownership Invariants

- `sd_card_manager` owns mount/unmount/recovery and rejects new VFS leases while
  recovering.
- `audio_wav` owns exactly one SD lease, FILE, and 4 KiB Internal-RAM read
  buffer for one open low-level stream.
- `audio_wav_prefetch` owns the low-level stream, two PSRAM slots, queues,
  worker task, and reopen/seek recovery state.
- `audio_manager` owns I2S TX, playback state, and the static DMA TX staging
  block.
- The prefetch worker writes only a FREE slot.
- The manager reads only a READY slot and returns it to the FREE queue after the
  complete block has been consumed.
- No caller closes a FILE, frees PSRAM, or deletes the worker while the reader
  can still access those resources.

## SD Recovery

On a confirmed low-level media/VFS read failure:

1. `audio_wav` reports the failure to `sd_card_manager`, closes the stale FILE,
   and releases its lease.
2. `audio_wav_prefetch` waits for the manager to become READY again.
3. It opens a fresh low-level stream, verifies the WAV metadata did not change,
   seeks to the last successfully copied data offset, and resumes filling the
   current PSRAM slot.
4. The current policy permits one recovery attempt per playback operation.

A missing file or malformed/unsupported WAV is not treated as physical SD media
failure.

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

## Phase 11.4.4 Hardware Acceptance

Hardware is still required before calling the path glitch-free or stable.
Validate at least canonical 5 s, 10 s, 11 s, 30 s, and 60 s WAV files with the
full Gateway services active.

Expected nominal results:

```text
WAV playback completes to EOF
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

The WAV prefetch/recovery work must not modify the proven microphone DSP, I2S
format, DMA geometry, startup discard, slot detection, or recorded-audio
conditioning.
