#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio_wav.h"

static unsigned s_release_count = 0U;

bool sd_card_manager_is_mounted(void) { return true; }
esp_err_t sd_card_manager_acquire(void) { return ESP_OK; }
void sd_card_manager_release(void) { ++s_release_count; }
void sd_card_manager_report_io_error(esp_err_t error) { (void)error; }
bool sd_card_manager_is_vfs_media_error(int error_number)
{
    (void)error_number;
    return false;
}

int main(void)
{
    enum { DATA_BYTES = 2048U, COMMITTED_BLOCK_BYTES = 512U };
    FILE *file = tmpfile();
    uint8_t *buffer = malloc(AUDIO_WAV_STREAM_BUFFER_BYTES);
    if ((file == NULL) || (buffer == NULL))
    {
        free(buffer);
        if (file != NULL) (void)fclose(file);
        return 1;
    }
    for (size_t index = 0U; index < DATA_BYTES; ++index)
    {
        (void)fputc((int)(index & 0xffU), file);
    }
    (void)fseek(file, 0L, SEEK_SET);

    audio_wav_stream_t stream = {
        .file = file,
        .buffer = buffer,
        .info = {
            .block_align = 2U,
            .data_size_bytes = DATA_BYTES,
            .data_offset = 0L,
        },
        .data_bytes_remaining = DATA_BYTES,
        .sd_lease_held = true,
    };

    bool ok =
        (audio_wav_stream_seek_data(&stream, COMMITTED_BLOCK_BYTES) == ESP_OK) &&
        (stream.data_bytes_read == COMMITTED_BLOCK_BYTES) &&
        (stream.data_bytes_remaining == DATA_BYTES - COMMITTED_BLOCK_BYTES) &&
        (ftell(file) == COMMITTED_BLOCK_BYTES) &&
        (audio_wav_stream_seek_data(&stream, COMMITTED_BLOCK_BYTES + 1U) ==
         ESP_ERR_INVALID_SIZE) &&
        (audio_wav_stream_seek_data(&stream, DATA_BYTES + 2U) ==
         ESP_ERR_INVALID_SIZE) &&
        (audio_wav_stream_close(&stream) == ESP_OK) &&
        (s_release_count == 1U);

    printf("[%s] WAV committed-position seek/bounds/lease\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
