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
 * @brief Small physical SD read quantum used by the target prefetch producer.
 *
 * Four KiB matches the current SDSPI transfer ceiling. The target firmware
 * repeatedly reads this amount into one of two larger PSRAM playback buffers.
 */
#define AUDIO_WAV_STORAGE_READ_BYTES  (4U * 1024U)

/** @brief Duration cached by each target-side ping-pong buffer. */
#define AUDIO_WAV_PREFETCH_SECONDS  10U

/** @brief Canonical PCM16 mono source byte rate: 16 kHz * 2 bytes. */
#define AUDIO_WAV_CANONICAL_BYTE_RATE  32000U

/**
 * @brief Capacity of each PSRAM ping-pong buffer.
 *
 * Each buffer stores ten seconds of canonical PCM16 mono audio. Two buffers
 * therefore reserve about 625 KiB of PSRAM while one WAV stream is open.
 */
#define AUDIO_WAV_PREFETCH_BUFFER_BYTES  \
    (AUDIO_WAV_CANONICAL_BYTE_RATE * AUDIO_WAV_PREFETCH_SECONDS)

/**
 * @brief Host-test compatibility buffer size.
 *
 * Native parser tests do not create the ESP-IDF prefetch task. They keep the
 * original small synchronous reader so the real RIFF parser remains testable
 * without FreeRTOS/PSRAM.
 */
#define AUDIO_WAV_STREAM_BUFFER_BYTES  AUDIO_WAV_STORAGE_READ_BYTES

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
 * @brief One private, caller-owned bounded WAV stream.
 *
 * On ESP-IDF targets, successful open creates a private producer context with
 * two PSRAM buffers. A dedicated low-priority reader task owns FILE/fread while
 * the audio-manager task consumes one completed buffer at a time. The opaque
 * context keeps FreeRTOS implementation types out of this private interface.
 *
 * `file` remains non-NULL while the stream is open and `buffer` is a non-NULL
 * compatibility/validity marker used by audio_manager. Callers must not read or
 * free either field directly.
 *
 * Native host tests use the original synchronous 4 KiB reader path so the real
 * RIFF parser can be compiled without ESP-IDF runtime headers.
 */
typedef struct
{
    FILE *file;
    uint8_t *buffer;
    audio_wav_info_t info;
    uint32_t data_bytes_remaining;
    uint64_t data_bytes_read;
    void *prefetch_context;
} audio_wav_stream_t;

/**
 * @brief Reset a closed stream before first use or reuse.
 *
 * The caller must close an open stream before resetting it.
 */
void audio_wav_stream_reset(audio_wav_stream_t *stream);

/**
 * @brief Validate that a path names a file below the SD VFS mount.
 *
 * This performs no filesystem access and is safe for manager command
 * validation before the path is copied. The caller still owns path.
 */
bool audio_wav_path_is_valid(const char *path);

/**
 * @brief Parse a RIFF/WAVE file and leave its position at the data payload.
 *
 * This parser is separate from opening so fixture tests can supply a normal
 * FILE without requiring SD mount state. The caller retains ownership of file.
 * Only PCM integer, mono, 16-kHz, 16-bit little-endian WAV data is accepted.
 *
 * @return ESP_OK for a valid canonical WAV, ESP_ERR_NOT_SUPPORTED for a valid
 *         but unsupported audio format, ESP_ERR_INVALID_RESPONSE when the
 *         existing file is not RIFF/WAVE or lacks fmt/data, or a deterministic
 *         argument, size, seek, or read error.
 */
esp_err_t audio_wav_parse_file(
    FILE *file,
    audio_wav_info_t *info);

/**
 * @brief Open and validate one WAV source on the mounted SD VFS path.
 *
 * path must be a non-empty absolute path rooted at SD_MOUNT_POINT. On target,
 * the helper allocates two 10-second PCM16 mono buffers in PSRAM, starts one
 * private SD prefetch task, and waits for the first buffer to become ready.
 * The producer immediately fills the alternate buffer while playback consumes
 * the first one; no fixed 70-percent trigger is required.
 *
 * @return ESP_ERR_NOT_FOUND only when the path cannot be found;
 *         audio_wav_parse_file() semantics apply after fopen succeeds.
 */
esp_err_t audio_wav_stream_open(
    audio_wav_stream_t *stream,
    const char *path);

/**
 * @brief Borrow the next completed PCM payload buffer.
 *
 * On target, the returned pointer refers to one complete ping-pong buffer (up
 * to ten seconds) that remains owned by the stream until the next read/close.
 * The alternate buffer is filled concurrently by the SD reader task. If the
 * consumer reaches the next buffer before it is ready, this call waits only for
 * a bounded interval and the event is recorded as a prefetch starvation.
 *
 * On ESP_OK, zero bytes_read means normal end of payload. On an I/O/prefetch
 * error, buffer and bytes_read are reset to NULL/zero.
 */
esp_err_t audio_wav_stream_read(
    audio_wav_stream_t *stream,
    const uint8_t **buffer,
    size_t *bytes_read);

/**
 * @brief Stop prefetch, close the stream, and release all playback buffers.
 *
 * Target cleanup cooperatively stops the producer and waits for it to leave
 * fread before closing FILE/freeing PSRAM, preventing use-after-free. This is
 * idempotent for a reset/closed stream. If the producer cannot stop within the
 * finite timeout, resources remain owned by the stream and ESP_ERR_TIMEOUT is
 * returned so the caller may retry cleanup safely.
 */
esp_err_t audio_wav_stream_close(audio_wav_stream_t *stream);
