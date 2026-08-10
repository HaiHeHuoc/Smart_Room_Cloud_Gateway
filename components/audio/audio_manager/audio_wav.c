/**
 * @file audio_wav.c
 * @brief Private bounded RIFF/WAVE reader for the mounted SD filesystem.
 */

/* Includes ----------------------------------------------------------------- */
#include "audio_wav.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "board_config.h"
#include "sd_card_manager.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/* Macros ------------------------------------------------------------------- */
#define AUDIO_WAV_RIFF_HEADER_BYTES       12U
#define AUDIO_WAV_CHUNK_HEADER_BYTES       8U
#define AUDIO_WAV_FMT_MIN_BYTES            16U
#define AUDIO_WAV_PCM_FORMAT                1U
#define AUDIO_WAV_MONO_CHANNELS             1U
#define AUDIO_WAV_SAMPLE_RATE_HZ        16000U
#define AUDIO_WAV_BITS_PER_SAMPLE          16U
#define AUDIO_WAV_BLOCK_ALIGN_BYTES         2U
#define AUDIO_WAV_BYTE_RATE             32000U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "AUDIO_WAV";

/* Function Prototypes ------------------------------------------------------ */
static uint16_t audio_wav_read_le16(const uint8_t *bytes);
static uint32_t audio_wav_read_le32(const uint8_t *bytes);
static esp_err_t audio_wav_read_exact(
    FILE *file,
    void *buffer,
    size_t bytes_to_read);
static esp_err_t audio_wav_seek_absolute(
    FILE *file,
    uint64_t offset);
static esp_err_t audio_wav_get_file_size(
    FILE *file,
    uint64_t *file_size);
static esp_err_t audio_wav_validate_format(audio_wav_info_t *info);
static esp_err_t audio_wav_open_error_from_errno(int error_number);

/* Static Functions --------------------------------------------------------- */
static uint16_t audio_wav_read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8U));
}

static uint32_t audio_wav_read_le32(const uint8_t *bytes)
{
    return (uint32_t)((uint32_t)bytes[0] |
                      ((uint32_t)bytes[1] << 8U) |
                      ((uint32_t)bytes[2] << 16U) |
                      ((uint32_t)bytes[3] << 24U));
}

static esp_err_t audio_wav_read_exact(
    FILE *file,
    void *buffer,
    size_t bytes_to_read)
{
    if ((file == NULL) || (buffer == NULL) || (bytes_to_read == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t bytes_read = fread(buffer, 1U, bytes_to_read, file);
    if (bytes_read == bytes_to_read)
    {
        return ESP_OK;
    }

    return ferror(file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
}

static esp_err_t audio_wav_seek_absolute(
    FILE *file,
    uint64_t offset)
{
    if (file == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (offset > (uint64_t)LONG_MAX)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (fseek(file, (long)offset, SEEK_SET) != 0)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t audio_wav_get_file_size(
    FILE *file,
    uint64_t *file_size)
{
    if ((file == NULL) || (file_size == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (fseek(file, 0L, SEEK_END) != 0)
    {
        return ESP_FAIL;
    }

    const long end_offset = ftell(file);
    if (end_offset < 0L)
    {
        return (errno == EOVERFLOW) ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
    }

    if (fseek(file, 0L, SEEK_SET) != 0)
    {
        return ESP_FAIL;
    }

    *file_size = (uint64_t)end_offset;
    return ESP_OK;
}

bool audio_wav_path_is_valid(const char *path)
{
    if ((path == NULL) || (path[0] == '\0'))
    {
        return false;
    }

    const size_t mount_length = strlen(SD_MOUNT_POINT);
    if (strncmp(path, SD_MOUNT_POINT, mount_length) != 0)
    {
        return false;
    }

    return (path[mount_length] == '/') && (path[mount_length + 1U] != '\0');
}

static esp_err_t audio_wav_validate_format(audio_wav_info_t *info)
{
    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((info->audio_format != AUDIO_WAV_PCM_FORMAT) ||
        (info->channels != AUDIO_WAV_MONO_CHANNELS) ||
        (info->sample_rate_hz != AUDIO_WAV_SAMPLE_RATE_HZ) ||
        (info->bits_per_sample != AUDIO_WAV_BITS_PER_SAMPLE))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if ((info->block_align != AUDIO_WAV_BLOCK_ALIGN_BYTES) ||
        (info->byte_rate != AUDIO_WAV_BYTE_RATE))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t audio_wav_open_error_from_errno(int error_number)
{
    if ((error_number == ENOENT) || (error_number == ENOTDIR))
    {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_FAIL;
}

/* Functions ---------------------------------------------------------------- */
void audio_wav_stream_reset(audio_wav_stream_t *stream)
{
    if (stream != NULL)
    {
        memset(stream, 0, sizeof(*stream));
    }
}

esp_err_t audio_wav_parse_file(
    FILE *file,
    audio_wav_info_t *info)
{
    if ((file == NULL) || (info == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *info = (audio_wav_info_t) {0};

    uint64_t file_size = 0U;
    esp_err_t result = audio_wav_get_file_size(file, &file_size);
    if (result != ESP_OK)
    {
        return result;
    }

    if (file_size < AUDIO_WAV_RIFF_HEADER_BYTES)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t riff_header[AUDIO_WAV_RIFF_HEADER_BYTES] = {0};
    result = audio_wav_read_exact(
        file,
        riff_header,
        sizeof(riff_header));
    if (result != ESP_OK)
    {
        return result;
    }

    if ((memcmp(riff_header, "RIFF", 4U) != 0) ||
        (memcmp(&riff_header[8], "WAVE", 4U) != 0))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t riff_size = audio_wav_read_le32(&riff_header[4]);
    if (riff_size < 4U)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint64_t riff_end_offset = 8U + (uint64_t)riff_size;
    if ((riff_end_offset < AUDIO_WAV_RIFF_HEADER_BYTES) ||
        (riff_end_offset > file_size))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    bool found_format = false;
    bool found_data = false;
    uint64_t chunk_offset = AUDIO_WAV_RIFF_HEADER_BYTES;

    while (chunk_offset < riff_end_offset)
    {
        if ((riff_end_offset - chunk_offset) < AUDIO_WAV_CHUNK_HEADER_BYTES)
        {
            return ESP_ERR_INVALID_SIZE;
        }

        uint8_t chunk_header[AUDIO_WAV_CHUNK_HEADER_BYTES] = {0};
        result = audio_wav_read_exact(
            file,
            chunk_header,
            sizeof(chunk_header));
        if (result != ESP_OK)
        {
            return result;
        }

        const uint32_t chunk_size = audio_wav_read_le32(&chunk_header[4]);
        const uint64_t chunk_data_offset =
            chunk_offset + AUDIO_WAV_CHUNK_HEADER_BYTES;
        const uint64_t chunk_padded_size =
            (uint64_t)chunk_size + (uint64_t)(chunk_size & 1U);
        const uint64_t next_chunk_offset =
            chunk_data_offset + chunk_padded_size;

        if ((next_chunk_offset < chunk_data_offset) ||
            (next_chunk_offset > riff_end_offset) ||
            (next_chunk_offset > file_size))
        {
            return ESP_ERR_INVALID_SIZE;
        }

        if (memcmp(chunk_header, "fmt ", 4U) == 0)
        {
            if (found_format || (chunk_size < AUDIO_WAV_FMT_MIN_BYTES))
            {
                return ESP_ERR_INVALID_RESPONSE;
            }

            uint8_t format_bytes[AUDIO_WAV_FMT_MIN_BYTES] = {0};
            result = audio_wav_read_exact(
                file,
                format_bytes,
                sizeof(format_bytes));
            if (result != ESP_OK)
            {
                return result;
            }

            info->audio_format = audio_wav_read_le16(&format_bytes[0]);
            info->channels = audio_wav_read_le16(&format_bytes[2]);
            info->sample_rate_hz = audio_wav_read_le32(&format_bytes[4]);
            info->byte_rate = audio_wav_read_le32(&format_bytes[8]);
            info->block_align = audio_wav_read_le16(&format_bytes[12]);
            info->bits_per_sample = audio_wav_read_le16(&format_bytes[14]);

            result = audio_wav_validate_format(info);
            if (result != ESP_OK)
            {
                return result;
            }

            found_format = true;
        }
        else if (memcmp(chunk_header, "data", 4U) == 0)
        {
            if (found_data || (chunk_size == 0U))
            {
                return ESP_ERR_INVALID_SIZE;
            }

            if (chunk_data_offset > (uint64_t)LONG_MAX)
            {
                return ESP_ERR_NOT_SUPPORTED;
            }

            info->data_offset = (long)chunk_data_offset;
            info->data_size_bytes = chunk_size;
            found_data = true;
        }

        result = audio_wav_seek_absolute(file, next_chunk_offset);
        if (result != ESP_OK)
        {
            return result;
        }

        chunk_offset = next_chunk_offset;

        if (found_format && found_data)
        {
            break;
        }
    }

    if (!found_format || !found_data)
    {
        /* The file exists, but its RIFF structure is not a valid WAV. */
        return ESP_ERR_INVALID_RESPONSE;
    }

    if ((info->data_size_bytes == 0U) ||
        ((info->data_size_bytes % info->block_align) != 0U))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    info->duration_ms = (uint32_t)(
        ((uint64_t)info->data_size_bytes * 1000U) / info->byte_rate);

    return audio_wav_seek_absolute(file, (uint64_t)info->data_offset);
}

esp_err_t audio_wav_stream_open(
    audio_wav_stream_t *stream,
    const char *path)
{
    if ((stream == NULL) || !audio_wav_path_is_valid(path))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((stream->file != NULL) || (stream->buffer != NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!sd_card_manager_is_mounted())
    {
        ESP_LOGW(TAG, "SD filesystem is not mounted for WAV path: %s", path);
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        const int open_errno = errno;
        ESP_LOGW(TAG, "Failed to open WAV %s: errno=%d", path, open_errno);
        return audio_wav_open_error_from_errno(open_errno);
    }

    if (setvbuf(file, NULL, _IONBF, 0) != 0)
    {
        ESP_LOGW(TAG, "Failed to configure unbuffered WAV I/O: %s", path);
        (void)fclose(file);
        return ESP_FAIL;
    }

    audio_wav_info_t info = {0};
    esp_err_t result = audio_wav_parse_file(file, &info);
    if (result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Rejected WAV %s: %s",
            path,
            esp_err_to_name(result));
        (void)fclose(file);
        return result;
    }

    uint8_t *const buffer = (uint8_t *)heap_caps_malloc(
        AUDIO_WAV_STREAM_BUFFER_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == NULL)
    {
        (void)fclose(file);
        return ESP_ERR_NO_MEM;
    }

    audio_wav_stream_reset(stream);
    stream->file = file;
    stream->buffer = buffer;
    stream->info = info;
    stream->data_bytes_remaining = info.data_size_bytes;

    ESP_LOGI(
        TAG,
        "WAV opened path=%s fmt=%u ch=%u rate=%u bits=%u data_offset=%ld data_bytes=%u duration=%ums",
        path,
        (unsigned)info.audio_format,
        (unsigned)info.channels,
        (unsigned)info.sample_rate_hz,
        (unsigned)info.bits_per_sample,
        info.data_offset,
        (unsigned)info.data_size_bytes,
        (unsigned)info.duration_ms);

    return ESP_OK;
}

esp_err_t audio_wav_stream_read(
    audio_wav_stream_t *stream,
    const uint8_t **buffer,
    size_t *bytes_read)
{
    if ((stream == NULL) || (buffer == NULL) || (bytes_read == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *buffer = NULL;
    *bytes_read = 0U;

    if ((stream->file == NULL) || (stream->buffer == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!sd_card_manager_is_mounted())
    {
        ESP_LOGW(TAG, "SD filesystem became unavailable during WAV read");
        const esp_err_t close_result = audio_wav_stream_close(stream);
        if (close_result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "WAV cleanup after unavailable SD failed: %s",
                esp_err_to_name(close_result));
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (stream->data_bytes_remaining == 0U)
    {
        return ESP_OK;
    }

    size_t requested_bytes = stream->data_bytes_remaining;
    if (requested_bytes > AUDIO_WAV_STREAM_BUFFER_BYTES)
    {
        requested_bytes = AUDIO_WAV_STREAM_BUFFER_BYTES;
    }

    const size_t received_bytes = fread(
        stream->buffer,
        1U,
        requested_bytes,
        stream->file);
    if (received_bytes != requested_bytes)
    {
        const esp_err_t result = ferror(stream->file)
                                     ? ESP_FAIL
                                     : ESP_ERR_INVALID_SIZE;
        ESP_LOGW(
            TAG,
            "WAV read failed: requested=%u received=%u error=%s",
            (unsigned)requested_bytes,
            (unsigned)received_bytes,
            esp_err_to_name(result));
        const esp_err_t close_result = audio_wav_stream_close(stream);
        if (close_result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "WAV cleanup after read failure failed: %s",
                esp_err_to_name(close_result));
        }
        return result;
    }

    stream->data_bytes_remaining -= (uint32_t)received_bytes;
    stream->data_bytes_read += received_bytes;
    *buffer = stream->buffer;
    *bytes_read = received_bytes;
    return ESP_OK;
}

esp_err_t audio_wav_stream_close(audio_wav_stream_t *stream)
{
    if (stream == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    if ((stream->file != NULL) && (fclose(stream->file) != 0))
    {
        ESP_LOGW(TAG, "Failed to close WAV file: errno=%d", errno);
        result = ESP_FAIL;
    }

    heap_caps_free(stream->buffer);
    audio_wav_stream_reset(stream);
    return result;
}
