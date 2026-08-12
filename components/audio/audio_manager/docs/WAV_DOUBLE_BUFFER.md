# WAV Ping-Pong Prefetch — Phase 11.4.4

## Goal

Prevent nominal SD/FATFS latency from directly starving I2S playback while
keeping the existing `audio_manager` TX path unchanged.

The target implementation uses two 10-second PCM16 mono buffers in PSRAM. A
private lower-priority prefetch task reads SD data through one reusable 4 KiB
Internal/DMA staging buffer and copies it into the currently free PSRAM buffer.
The `audio_manager` task remains the sole I2S owner and consumes only completed
PSRAM buffers.

## Data Flow

```text
SD / FATFS
    |
    | fread <= 4 KiB
    v
4 KiB Internal/DMA staging
    |
    | memcpy
    v
+-------------------+      +-------------------+
| PSRAM Buffer A    |      | PSRAM Buffer B    |
| 320,000 bytes     |      | 320,000 bytes     |
| ~10 s PCM16 mono  |      | ~10 s PCM16 mono  |
+-------------------+      +-------------------+
          |                         |
          +------ ping-pong --------+
                    |
                    v
           audio_manager task
                    |
           mono -> stereo staging
                    |
                    v
              existing I2S TX
                    |
                    v
                MAX98357A
```

The producer starts filling the alternate buffer as soon as possible. It does
not wait for a fixed 70-percent playback threshold. This gives the producer up
to approximately one full buffer duration to prepare the next block.

## Memory Model

Canonical WAV format remains:

- PCM integer
- mono
- 16 kHz
- 16 bit
- 32,000 bytes/s

Target runtime allocations per open WAV stream:

```text
PSRAM Buffer A        320,000 bytes
PSRAM Buffer B        320,000 bytes
Internal/DMA staging    4,096 bytes
prefetch context          small Internal allocation
prefetch task stack       4,096 bytes (FreeRTOS task stack allocation)
```

Bulk audio remains in PSRAM. I2S TX staging remains the existing static
Internal/DMA block owned by `audio_manager`.

## Ownership

- `audio_manager` task owns I2S TX and playback state.
- `audio_wav` prefetch task owns `FILE`/`fread` while active.
- The producer only writes the FREE PSRAM buffer.
- The consumer only reads the READY/BORROWED PSRAM buffer.
- `audio_wav_stream_close()` requests producer stop and waits for the producer
  to leave `fread` before closing `FILE` or freeing buffers.
- No task may free a buffer that the other task can still access.

## Cancellation

`audio_manager_stop_playback()` remains cooperative.

The audio manager checks cancellation between existing 256-frame TX blocks.
When playback exits, stream cleanup sets the prefetch STOP event. The producer
checks STOP between each 4 KiB storage read, so cancellation does not need to
wait for an entire 10-second prefetch operation.

A pathological storage transaction that never returns can still cause the
bounded close/manager-stop timeout. Resources are deliberately retained rather
than force-freed in that case to avoid use-after-free.

## Diagnostics

At stream cleanup, `AUDIO_WAV` emits:

```text
WAV_PREFETCH_DIAG
storage_reads=<count>
max_fread_us=<maximum successful/attempted 4KiB fread duration>
consumer_waits=<times playback reached the next buffer before it was ready>
max_wait_us=<maximum next-buffer wait>
```

For nominal glitch-free playback, the strongest software-side signal is:

```text
consumer_waits = 0
```

`consumer_waits > 0` means the audio consumer reached a ping-pong boundary
before the producer had the next buffer ready. This is a real prefetch
starvation event and should be correlated with audible output and I2S metrics.

## Phase 11.4.4 Hardware Acceptance

Test at least canonical 5 s, 30 s, and 60 s WAV files with the full Gateway
services active.

Expected nominal results:

```text
WAV playback completes to EOF
consumer_waits = 0
tx_timeout = 0
tx_partial = 0
tx_q_ovf = 0
no audible gap at 10-second boundaries
no WDT/crash
PLAYBACK -> IDLE
speaker DIN returns LOW
```

Also test cancellation:

```text
cancel near 1 s
cancel near a 10 s buffer boundary
cancel in the middle of a 60 s WAV
manager stop during WAV playback
```

Expected:

```text
no stale FILE
no leaked PSRAM buffer
no leaked prefetch task
no live I2S TX after cleanup
controlled cancel returns to IDLE
```

Finally run the golden MIC regression after WAV testing:

```text
INMP441 record -> DSP -> recorded playback
```

The WAV prefetch change must not modify the proven microphone DSP, I2S format,
DMA geometry, slot detection, startup discard, or recorded-audio conditioning.
