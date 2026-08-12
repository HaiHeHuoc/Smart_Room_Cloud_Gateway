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
 * @brief Reusable PCM payload buffer size for one WAV stream.
 *
 * Four KiB matches the current SDSPI max transfer size and represents 128 ms
 * of the canonical 16-kHz, PCM16 mono source. It is allocated once when a
 * stream opens and is never resized or allocated per read.
 */
#define AUDIO_WAV_STREAM_BUFFER_BYTES  (4U * 1024U)

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
 * The FILE handle, SD VFS lease, and buffer are owned by this structure between
 * successful audio_wav_stream_open() and audio_wav_stream_close().
 * Zero-initialize it or call audio_wav_stream_reset() before first use. It is
 * not thread-safe; the private prefetch reader must serialize open/read/close
 * operations and join before any lifecycle cleanup frees this stream.
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
 * path must be a non-empty absolute path rooted at SD_MOUNT_POINT. The helper
 * atomically acquires one SD VFS lease before opening the read-only FILE,
 * parses it, and allocates its fixed reusable read buffer once. The lease is
 * released only after fclose(), so SD recovery cannot unmount the VFS while
 * the reader still owns the handle.
 *
 * @return ESP_ERR_NOT_FOUND only when the path cannot be found;
 *         audio_wav_parse_file() semantics apply after fopen succeeds.
 */
esp_err_t audio_wav_stream_open(
    audio_wav_stream_t *stream,
    const char *path);

/**
 * @brief Read the next bounded PCM payload from an opened stream.
 *
 * On ESP_OK, buffer receives a stream-owned little-endian PCM16 byte buffer,
 * valid until the next read or close. A zero bytes_read then means normal end
 * of payload. On error, buffer and bytes_read are reset to NULL/zero and any
 * media/read failure closes the stream before this function returns. A real
 * VFS I/O failure is reported to sd_card_manager before the lease is released.
 */
esp_err_t audio_wav_stream_read(
    audio_wav_stream_t *stream,
    const uint8_t **buffer,
    size_t *bytes_read);

/**
 * @brief Read at most max_bytes of the next bounded PCM payload.
 *
 * This has the same ownership/error semantics as audio_wav_stream_read(), but
 * lets the prefetcher end a logical PSRAM block exactly at its configured
 * boundary. max_bytes is capped at AUDIO_WAV_STREAM_BUFFER_BYTES.
 */
esp_err_t audio_wav_stream_read_limited(
    audio_wav_stream_t *stream,
    size_t max_bytes,
    const uint8_t **buffer,
    size_t *bytes_read);

/**
 * @brief Seek an opened stream to one committed PCM payload offset.
 *
 * data_offset_bytes is relative to the beginning of the validated data chunk,
 * must not exceed data_size_bytes, and must preserve block alignment. On
 * success the stream read/remaining counters are restored to that position.
 * A VFS seek failure is reported to sd_card_manager, but the caller still owns
 * the stream and must close it before waiting for SD recovery.
 *
 * This helper exists for a fresh FILE opened after media recovery. A FILE that
 * has already returned a FatFS read error must be closed, never clearerr()/seek
 * retried, because ESP-IDF FatFS retains the error in its FIL object.
 */
esp_err_t audio_wav_stream_seek_data(
    audio_wav_stream_t *stream,
    uint64_t data_offset_bytes);

/**
 * @brief Close an opened stream and release its reusable buffer.
 *
 * This is idempotent for a reset/closed stream. It releases local resources
 * even when fclose() reports an error, and releases the SD VFS lease only
 * after the FILE has been closed.
 */
esp_err_t audio_wav_stream_close(audio_wav_stream_t *stream);
