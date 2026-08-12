#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio_wav.h"
#include "esp_err.h"

static unsigned s_release_calls = 0U;
static unsigned s_report_error_calls = 0U;
static bool s_mounted = true;

bool sd_card_manager_is_mounted(void)
{
    return s_mounted;
}

esp_err_t sd_card_manager_acquire(void)
{
    return s_mounted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void sd_card_manager_release(void)
{
    ++s_release_calls;
}

void sd_card_manager_report_io_error(esp_err_t error)
{
    (void)error;
    ++s_report_error_calls;
}

bool sd_card_manager_is_vfs_media_error(int error_number)
{
    (void)error_number;
    return false;
}

static bool write_payload(FILE *file, size_t byte_count)
{
    for (size_t index = 0U; index < byte_count; ++index)
    {
        if (fputc((int)(index & 0xffU), file) == EOF)
        {
            return false;
        }
    }

    return fseek(file, 0L, SEEK_SET) == 0;
}

static bool test_bounded_read_seek_and_close(void)
{
    enum { PAYLOAD_BYTES = 8192U };

    FILE *file = tmpfile();
    if ((file == NULL) || !write_payload(file, PAYLOAD_BYTES))
    {
        if (file != NULL)
        {
            (void)fclose(file);
        }
        return false;
    }

    uint8_t *buffer = (uint8_t *)malloc(AUDIO_WAV_STREAM_BUFFER_BYTES);
    if (buffer == NULL)
    {
        (void)fclose(file);
        return false;
    }

    audio_wav_stream_t stream = {
        .file = file,
        .buffer = buffer,
        .info = {
            .block_align = 2U,
            .data_size_bytes = PAYLOAD_BYTES,
            .data_offset = 0L,
        },
        .data_bytes_remaining = PAYLOAD_BYTES,
        .data_bytes_read = 0U,
        .sd_lease_held = true,
    };

    const uint8_t *read_buffer = NULL;
    size_t bytes_read = 0U;

    esp_err_t result = audio_wav_stream_read_limited(
        &stream,
        AUDIO_WAV_STREAM_BUFFER_BYTES + 2048U,
        &read_buffer,
        &bytes_read);
    if ((result != ESP_OK) ||
        (read_buffer != buffer) ||
        (bytes_read != AUDIO_WAV_STREAM_BUFFER_BYTES) ||
        (stream.data_bytes_read != AUDIO_WAV_STREAM_BUFFER_BYTES) ||
        (stream.data_bytes_remaining !=
         (PAYLOAD_BYTES - AUDIO_WAV_STREAM_BUFFER_BYTES)) ||
        (s_release_calls != 0U))
    {
        (void)audio_wav_stream_close(&stream);
        return false;
    }

    result = audio_wav_stream_read_limited(
        &stream,
        1000U,
        &read_buffer,
        &bytes_read);
    if ((result != ESP_OK) || (bytes_read != 1000U) ||
        (stream.data_bytes_read !=
         (AUDIO_WAV_STREAM_BUFFER_BYTES + 1000U)))
    {
        (void)audio_wav_stream_close(&stream);
        return false;
    }

    result = audio_wav_stream_seek_data(&stream, 2000U);
    if ((result != ESP_OK) ||
        (ftell(file) != 2000L) ||
        (stream.data_bytes_read != 2000U) ||
        (stream.data_bytes_remaining != (PAYLOAD_BYTES - 2000U)))
    {
        (void)audio_wav_stream_close(&stream);
        return false;
    }

    result = audio_wav_stream_close(&stream);
    if ((result != ESP_OK) ||
        (s_release_calls != 1U) ||
        (s_report_error_calls != 0U) ||
        (stream.file != NULL) ||
        (stream.buffer != NULL) ||
        stream.sd_lease_held)
    {
        return false;
    }

    /* Closed-stream cleanup must be idempotent and must not release twice. */
    result = audio_wav_stream_close(&stream);
    return (result == ESP_OK) &&
           (s_release_calls == 1U) &&
           (s_report_error_calls == 0U);
}

static bool test_recovery_state_closes_and_releases(void)
{
    FILE *file = tmpfile();
    if ((file == NULL) || !write_payload(file, 16U))
    {
        if (file != NULL)
        {
            (void)fclose(file);
        }
        return false;
    }

    uint8_t *buffer = (uint8_t *)malloc(AUDIO_WAV_STREAM_BUFFER_BYTES);
    if (buffer == NULL)
    {
        (void)fclose(file);
        return false;
    }

    audio_wav_stream_t stream = {
        .file = file,
        .buffer = buffer,
        .info = {
            .block_align = 2U,
            .data_size_bytes = 16U,
            .data_offset = 0L,
        },
        .data_bytes_remaining = 16U,
        .sd_lease_held = true,
    };

    const unsigned releases_before = s_release_calls;
    s_mounted = false;

    const uint8_t *read_buffer = NULL;
    size_t bytes_read = 0U;
    const esp_err_t result = audio_wav_stream_read_limited(
        &stream,
        16U,
        &read_buffer,
        &bytes_read);

    s_mounted = true;

    return (result == ESP_ERR_INVALID_STATE) &&
           (read_buffer == NULL) &&
           (bytes_read == 0U) &&
           (s_release_calls == (releases_before + 1U)) &&
           (stream.file == NULL) &&
           (stream.buffer == NULL) &&
           !stream.sd_lease_held;
}

int main(void)
{
    const bool bounded_ok = test_bounded_read_seek_and_close();
    printf("[%s] bounded read/seek/lease contract\n",
           bounded_ok ? "PASS" : "FAIL");

    const bool recovery_ok = test_recovery_state_closes_and_releases();
    printf("[%s] recovery-state cleanup contract\n",
           recovery_ok ? "PASS" : "FAIL");

    return (bounded_ok && recovery_ok) ? 0 : 1;
}
