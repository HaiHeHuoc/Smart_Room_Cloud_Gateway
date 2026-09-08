/**
 * @file audio_wav.h
 * @brief Private bounded RIFF/WAVE PCM16 reader for audio_manager.
 *
 * This header is intentionally private to the audio_manager component. It
 * exposes no LVGL, I2S, FATFS, or public audio_manager API types.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

/**
 * @brief Reusable raw PCM read-buffer size for one WAV stream.
 *
 * Four KiB matches the current SDSPI transfer ceiling. The low-level WAV reader
 * remains synchronous and bounded; the separate audio_wav_prefetch layer owns
 * the two large PSRAM playback slots and its worker task.
 */
#define AUDIO_WAV_STREAM_BUFFER_BYTES  (4U * 1024U)

/** @brief Canonical PCM16 mono source byte rate: 16 kHz * 2 bytes. */
#define AUDIO_WAV_CANONICAL_BYTE_RATE  32000U

/** Maximum absolute magnitude representable by signed PCM16. */
#define AUDIO_WAV_PCM16_ABSOLUTE_MAX   32768U

/** Fixed-point unit used by the fixed full-scale WAV attenuation. */
#define AUDIO_WAV_PCM16_GAIN_Q16_ONE   (1U << 16U)

/**
 * Q16 gain that maps the full signed PCM16 range to target_peak_pcm16.
 *
 * target_peak_pcm16 must be in 1..32767. For the shared target of 9000,
 * this evaluates to 18000, exactly representing 9000 / 32768.
 */
#define AUDIO_WAV_PCM16_FULL_SCALE_GAIN_Q16(target_peak_pcm16) \
    ((uint32_t)((target_peak_pcm16) * 2U))

/** @brief Metadata retained after validating the canonical WAV format. */
typedef struct
{
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate_hz;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint32_t data_size_bytes;
    long data_offset;
    uint32_t duration_ms;
} audio_wav_info_t;

/**
 * @brief One private bounded WAV stream.
 *
 * The stream owns one FILE handle, one fixed 4 KiB Internal-RAM read buffer,
 * and one sd_card_manager VFS lease between successful open and close. It is
 * intentionally not thread-safe; its caller must serialize open/read/seek/close.
 *
 * The higher-level audio_wav_prefetch worker is the sole runtime caller for
 * production WAV streaming. Keeping prefetch outside this type avoids nested
 * producer tasks and keeps SD/FATFS ownership explicit.
 */
typedef struct
{
    FILE *file;
    uint8_t *buffer;
    audio_wav_info_t info;
    uint32_t data_bytes_remaining;
    uint64_t data_bytes_read;
    bool sd_lease_held;
} audio_wav_stream_t;

/** Reset a closed stream before first use or reuse. */
void audio_wav_stream_reset(audio_wav_stream_t *stream);

/**
 * @brief Validate that a path names a file below the SD VFS mount.
 *
 * This performs no filesystem access. The caller retains ownership of path.
 */
bool audio_wav_path_is_valid(const char *path);

/**
 * @brief Parse a RIFF/WAVE file and leave its position at the data payload.
 *
 * Only PCM integer, mono, 16-kHz, 16-bit little-endian WAV data is accepted.
 * The caller retains ownership of file.
 */
esp_err_t audio_wav_parse_file(
    FILE *file,
    audio_wav_info_t *info);

/**
 * @brief Acquire one SD VFS lease, open/validate a WAV, and allocate 4 KiB.
 *
 * On success the stream owns the lease until audio_wav_stream_close(). No task
 * or PSRAM playback slot is created by this function.
 */
esp_err_t audio_wav_stream_open(
    audio_wav_stream_t *stream,
    const char *path);

/** Read the next payload chunk, bounded by AUDIO_WAV_STREAM_BUFFER_BYTES. */
esp_err_t audio_wav_stream_read(
    audio_wav_stream_t *stream,
    const uint8_t **buffer,
    size_t *bytes_read);

/**
 * @brief Read at most max_bytes from the WAV data payload.
 *
 * The actual physical read remains capped at AUDIO_WAV_STREAM_BUFFER_BYTES.
 * On a confirmed media/VFS failure the helper reports recovery, closes the
 * stale FILE, releases its SD lease, and returns the original read error so a
 * higher-level owner can reopen a fresh stream safely.
 */
esp_err_t audio_wav_stream_read_limited(
    audio_wav_stream_t *stream,
    size_t max_bytes,
    const uint8_t **buffer,
    size_t *bytes_read);

/**
 * @brief Apply one fixed PCM16 full-scale-to-target mapping with rounding.
 *
 * This is a stateless per-sample scale, not a file scan or dynamic limiter.
 * It maps -32768 to -target_peak_pcm16 and never exceeds either target limit.
 * Invalid targets return zero. The caller applies its user volume afterward.
 */
int16_t audio_wav_pcm16_scale_full_range_to_peak(
    int16_t sample_pcm16,
    uint32_t target_peak_pcm16);

/**
 * @brief Seek to one block-aligned byte offset within the WAV data payload.
 *
 * Used by the prefetch recovery path after reopening a fresh FILE. The offset
 * is relative to the beginning of the data chunk, not to the RIFF file.
 */
esp_err_t audio_wav_stream_seek_data(
    audio_wav_stream_t *stream,
    uint64_t data_offset_bytes);

/**
 * @brief Close the FILE, free the bounded buffer, and release the SD lease.
 *
 * Idempotent for a reset/closed stream. Local ownership is released even when
 * fclose reports an error; the close failure is also reported to SD recovery.
 */
esp_err_t audio_wav_stream_close(audio_wav_stream_t *stream);
