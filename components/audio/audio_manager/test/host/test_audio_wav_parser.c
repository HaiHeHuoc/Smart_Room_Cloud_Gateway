#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_wav.h"
#include "esp_err.h"

typedef struct
{
    const char *riff_magic;
    const char *wave_magic;
    bool include_format;
    bool include_data;
    bool data_before_format;
    bool include_junk;
    bool include_list;
    bool include_unknown;
    bool include_odd_unknown;
    bool include_oversized_chunk;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate_hz;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint32_t data_declared_bytes;
    uint32_t data_actual_bytes;
} wav_fixture_t;

static unsigned s_tests_run = 0U;
static unsigned s_tests_failed = 0U;

bool sd_card_manager_is_mounted(void)
{
    return true;
}

static wav_fixture_t canonical_fixture(void)
{
    return (wav_fixture_t) {
        .riff_magic = "RIFF",
        .wave_magic = "WAVE",
        .include_format = true,
        .include_data = true,
        .audio_format = 1U,
        .channels = 1U,
        .sample_rate_hz = 16000U,
        .byte_rate = 32000U,
        .block_align = 2U,
        .bits_per_sample = 16U,
        .data_declared_bytes = 4U,
        .data_actual_bytes = 4U,
    };
}

static bool write_bytes(FILE *file, const void *bytes, size_t byte_count)
{
    return fwrite(bytes, 1U, byte_count, file) == byte_count;
}

static bool write_le16(FILE *file, uint16_t value)
{
    const uint8_t bytes[2] = {
        (uint8_t)(value & 0xffU),
        (uint8_t)((value >> 8U) & 0xffU),
    };
    return write_bytes(file, bytes, sizeof(bytes));
}

static bool write_le32(FILE *file, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)(value & 0xffU),
        (uint8_t)((value >> 8U) & 0xffU),
        (uint8_t)((value >> 16U) & 0xffU),
        (uint8_t)((value >> 24U) & 0xffU),
    };
    return write_bytes(file, bytes, sizeof(bytes));
}

static bool write_chunk_header(
    FILE *file,
    const char chunk_id[4],
    uint32_t data_bytes)
{
    return write_bytes(file, chunk_id, 4U) && write_le32(file, data_bytes);
}

static bool write_zero_payload(FILE *file, uint32_t byte_count)
{
    if (byte_count == 0U)
    {
        return true;
    }

    if (fseek(file, (long)byte_count - 1L, SEEK_CUR) != 0)
    {
        return false;
    }

    return fputc(0, file) != EOF;
}

static bool write_format_chunk(FILE *file, const wav_fixture_t *fixture)
{
    return write_chunk_header(file, "fmt ", 16U) &&
           write_le16(file, fixture->audio_format) &&
           write_le16(file, fixture->channels) &&
           write_le32(file, fixture->sample_rate_hz) &&
           write_le32(file, fixture->byte_rate) &&
           write_le16(file, fixture->block_align) &&
           write_le16(file, fixture->bits_per_sample);
}

static bool write_data_chunk(FILE *file, const wav_fixture_t *fixture)
{
    return write_chunk_header(
               file,
               "data",
               fixture->data_declared_bytes) &&
           write_zero_payload(file, fixture->data_actual_bytes);
}

static bool write_metadata_chunk(
    FILE *file,
    const char chunk_id[4],
    uint32_t data_bytes)
{
    if (!write_chunk_header(file, chunk_id, data_bytes) ||
        !write_zero_payload(file, data_bytes))
    {
        return false;
    }

    return ((data_bytes & 1U) == 0U) || (fputc(0, file) != EOF);
}

static bool finish_riff_file(FILE *file)
{
    const long end_offset = ftell(file);
    if ((end_offset < 12L) || (fseek(file, 4L, SEEK_SET) != 0))
    {
        return false;
    }

    if (!write_le32(file, (uint32_t)end_offset - 8U))
    {
        return false;
    }

    return fseek(file, 0L, SEEK_SET) == 0;
}

static bool build_fixture(FILE *file, const wav_fixture_t *fixture)
{
    if (!write_bytes(file, fixture->riff_magic, 4U) ||
        !write_le32(file, 0U) ||
        !write_bytes(file, fixture->wave_magic, 4U))
    {
        return false;
    }

    if (fixture->include_junk &&
        !write_metadata_chunk(file, "JUNK", 8U))
    {
        return false;
    }
    if (fixture->include_list &&
        !write_metadata_chunk(file, "LIST", 12U))
    {
        return false;
    }
    if (fixture->include_unknown &&
        !write_metadata_chunk(file, "meta", 6U))
    {
        return false;
    }
    if (fixture->include_odd_unknown &&
        !write_metadata_chunk(file, "odd!", 3U))
    {
        return false;
    }
    if (fixture->include_oversized_chunk)
    {
        if (!write_chunk_header(file, "huge", 1024U) ||
            !write_zero_payload(file, 2U))
        {
            return false;
        }
        return finish_riff_file(file);
    }

    if (fixture->data_before_format && fixture->include_data &&
        !write_data_chunk(file, fixture))
    {
        return false;
    }
    if (fixture->include_format && !write_format_chunk(file, fixture))
    {
        return false;
    }
    if (!fixture->data_before_format && fixture->include_data &&
        !write_data_chunk(file, fixture))
    {
        return false;
    }

    return finish_riff_file(file);
}

static void record_result(
    const char *name,
    bool passed,
    esp_err_t actual,
    esp_err_t expected)
{
    ++s_tests_run;
    if (passed)
    {
        printf("[PASS] %s\n", name);
        return;
    }

    ++s_tests_failed;
    printf(
        "[FAIL] %s: actual=0x%x expected=0x%x\n",
        name,
        actual,
        expected);
}

static void run_fixture_test(
    const char *name,
    const wav_fixture_t *fixture,
    esp_err_t expected_result)
{
    FILE *file = tmpfile();
    if (file == NULL)
    {
        record_result(name, false, ESP_FAIL, expected_result);
        return;
    }

    if (!build_fixture(file, fixture))
    {
        (void)fclose(file);
        record_result(name, false, ESP_FAIL, expected_result);
        return;
    }

    audio_wav_info_t info = {0};
    const esp_err_t actual_result = audio_wav_parse_file(file, &info);
    bool passed = actual_result == expected_result;

    if (passed && (actual_result == ESP_OK))
    {
        const long current_offset = ftell(file);
        passed =
            (info.audio_format == 1U) &&
            (info.channels == 1U) &&
            (info.sample_rate_hz == 16000U) &&
            (info.byte_rate == 32000U) &&
            (info.block_align == 2U) &&
            (info.bits_per_sample == 16U) &&
            (info.data_size_bytes == fixture->data_declared_bytes) &&
            (info.duration_ms ==
             (uint32_t)(((uint64_t)fixture->data_declared_bytes * 1000U) /
                        32000U)) &&
            (current_offset == info.data_offset);
    }

    (void)fclose(file);
    record_result(name, passed, actual_result, expected_result);
}

static void run_raw_file_test(
    const char *name,
    const uint8_t *bytes,
    size_t byte_count,
    esp_err_t expected_result)
{
    FILE *file = tmpfile();
    if ((file == NULL) ||
        !write_bytes(file, bytes, byte_count) ||
        (fseek(file, 0L, SEEK_SET) != 0))
    {
        if (file != NULL)
        {
            (void)fclose(file);
        }
        record_result(name, false, ESP_FAIL, expected_result);
        return;
    }

    audio_wav_info_t info = {0};
    const esp_err_t actual_result = audio_wav_parse_file(file, &info);
    (void)fclose(file);
    record_result(
        name,
        actual_result == expected_result,
        actual_result,
        expected_result);
}

int main(void)
{
    audio_wav_stream_t missing_stream = {0};
    const esp_err_t missing_file_result = audio_wav_stream_open(
        &missing_stream,
        "/sdcard/__audio_wav_fixture_file_does_not_exist__.wav");
    record_result(
        "missing path maps to not found",
        missing_file_result == ESP_ERR_NOT_FOUND,
        missing_file_result,
        ESP_ERR_NOT_FOUND);

    wav_fixture_t fixture = canonical_fixture();
    run_fixture_test("canonical PCM16 mono 16 kHz", &fixture, ESP_OK);

    fixture = canonical_fixture();
    fixture.include_junk = true;
    run_fixture_test("JUNK chunk", &fixture, ESP_OK);

    fixture = canonical_fixture();
    fixture.include_list = true;
    run_fixture_test("LIST chunk", &fixture, ESP_OK);

    fixture = canonical_fixture();
    fixture.include_unknown = true;
    run_fixture_test("unknown metadata chunk", &fixture, ESP_OK);

    fixture = canonical_fixture();
    fixture.include_odd_unknown = true;
    run_fixture_test("odd unknown chunk with padding", &fixture, ESP_OK);

    fixture = canonical_fixture();
    fixture.data_before_format = true;
    run_fixture_test("data chunk before fmt chunk", &fixture, ESP_OK);

    fixture = canonical_fixture();
    fixture.data_declared_bytes = 1024U * 1024U;
    fixture.data_actual_bytes = fixture.data_declared_bytes;
    run_fixture_test("large canonical metadata", &fixture, ESP_OK);

    static const uint8_t random_file[16] = {
        0xdeU, 0xadU, 0xbeU, 0xefU, 0x01U, 0x02U, 0x03U, 0x04U,
        0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0aU, 0x0bU, 0x0cU,
    };
    run_raw_file_test(
        "random non-WAV file",
        random_file,
        sizeof(random_file),
        ESP_ERR_INVALID_RESPONSE);

    static const uint8_t truncated_header[8] = {
        'R', 'I', 'F', 'F', 0U, 0U, 0U, 0U,
    };
    run_raw_file_test(
        "truncated RIFF header",
        truncated_header,
        sizeof(truncated_header),
        ESP_ERR_INVALID_SIZE);

    fixture = canonical_fixture();
    fixture.riff_magic = "NOPE";
    run_fixture_test("invalid RIFF magic", &fixture, ESP_ERR_INVALID_RESPONSE);

    fixture = canonical_fixture();
    fixture.wave_magic = "NOPE";
    run_fixture_test("invalid WAVE magic", &fixture, ESP_ERR_INVALID_RESPONSE);

    fixture = canonical_fixture();
    fixture.include_format = false;
    run_fixture_test("missing fmt chunk", &fixture, ESP_ERR_INVALID_RESPONSE);

    fixture = canonical_fixture();
    fixture.include_data = false;
    run_fixture_test("missing data chunk", &fixture, ESP_ERR_INVALID_RESPONSE);

    fixture = canonical_fixture();
    fixture.data_declared_bytes = 0U;
    fixture.data_actual_bytes = 0U;
    run_fixture_test("zero-length data", &fixture, ESP_ERR_INVALID_SIZE);

    fixture = canonical_fixture();
    fixture.include_oversized_chunk = true;
    run_fixture_test("oversized truncated chunk", &fixture, ESP_ERR_INVALID_SIZE);

    fixture = canonical_fixture();
    fixture.channels = 2U;
    fixture.block_align = 4U;
    fixture.byte_rate = 64000U;
    run_fixture_test("stereo PCM16", &fixture, ESP_ERR_NOT_SUPPORTED);

    fixture = canonical_fixture();
    fixture.sample_rate_hz = 44100U;
    fixture.byte_rate = 88200U;
    run_fixture_test("44.1 kHz PCM16", &fixture, ESP_ERR_NOT_SUPPORTED);

    fixture = canonical_fixture();
    fixture.bits_per_sample = 24U;
    fixture.block_align = 3U;
    fixture.byte_rate = 48000U;
    fixture.data_declared_bytes = 6U;
    fixture.data_actual_bytes = 6U;
    run_fixture_test("24-bit PCM", &fixture, ESP_ERR_NOT_SUPPORTED);

    fixture = canonical_fixture();
    fixture.audio_format = 3U;
    run_fixture_test("float WAV", &fixture, ESP_ERR_NOT_SUPPORTED);

    fixture = canonical_fixture();
    fixture.block_align = 1U;
    run_fixture_test("invalid block_align", &fixture, ESP_ERR_INVALID_RESPONSE);

    fixture = canonical_fixture();
    fixture.byte_rate = 12345U;
    run_fixture_test("invalid byte_rate", &fixture, ESP_ERR_INVALID_RESPONSE);

    fixture = canonical_fixture();
    fixture.data_declared_bytes = 4U;
    fixture.data_actual_bytes = 2U;
    run_fixture_test("truncated data payload", &fixture, ESP_ERR_INVALID_SIZE);

    printf(
        "audio_wav parser tests: %u passed, %u failed, %u total\n",
        s_tests_run - s_tests_failed,
        s_tests_failed,
        s_tests_run);

    return (s_tests_failed == 0U) ? 0 : 1;
}
