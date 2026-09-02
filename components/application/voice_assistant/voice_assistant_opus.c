#include "voice_assistant_opus.h"

#include <limits.h>

#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_audio_types.h"
#include "esp_log.h"

#define VOICE_ASSISTANT_OPUS_BITRATE_BPS 24000
#define VOICE_ASSISTANT_OPUS_COMPLEXITY  5

static const char *const TAG = "VOICE_OPUS";
static void *s_encoder = NULL;
static void *s_decoder = NULL;
static int s_encoder_input_bytes = 0;
static int s_encoder_output_bytes = 0;

static esp_err_t opus_error_to_esp(esp_audio_err_t error)
{
    switch (error) {
        case ESP_AUDIO_ERR_OK:
            return ESP_OK;
        case ESP_AUDIO_ERR_MEM_LACK:
            return ESP_ERR_NO_MEM;
        case ESP_AUDIO_ERR_INVALID_PARAMETER:
            return ESP_ERR_INVALID_ARG;
        case ESP_AUDIO_ERR_BUFF_NOT_ENOUGH:
            return ESP_ERR_INVALID_SIZE;
        default:
            return ESP_FAIL;
    }
}

esp_err_t voice_assistant_opus_encoder_init(void)
{
    if (s_encoder != NULL) {
        return ESP_OK;
    }

    esp_opus_enc_config_t config = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = VOICE_ASSISTANT_OPUS_BITRATE_BPS,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity = VOICE_ASSISTANT_OPUS_COMPLEXITY,
        .enable_fec = false,
        .enable_dtx = false,
        .enable_vbr = false,
    };
    esp_audio_err_t codec_ret =
        esp_opus_enc_open(&config, sizeof(config), &s_encoder);
    if (codec_ret != ESP_AUDIO_ERR_OK) {
        s_encoder = NULL;
        ESP_LOGE(TAG, "encoder open failed codec_error=%d", (int)codec_ret);
        return opus_error_to_esp(codec_ret);
    }

    codec_ret = esp_opus_enc_get_frame_size(
        s_encoder, &s_encoder_input_bytes, &s_encoder_output_bytes);
    if ((codec_ret != ESP_AUDIO_ERR_OK) ||
        (s_encoder_input_bytes != (int)VOICE_ASSISTANT_OPUS_PCM_BYTES) ||
        (s_encoder_output_bytes <= 0) ||
        (s_encoder_output_bytes > (int)VOICE_ASSISTANT_OPUS_MAX_PACKET_BYTES)) {
        ESP_LOGE(TAG,
                 "encoder frame contract invalid codec_error=%d input=%d output=%d",
                 (int)codec_ret,
                 s_encoder_input_bytes,
                 s_encoder_output_bytes);
        esp_opus_enc_close(s_encoder);
        s_encoder = NULL;
        return (codec_ret == ESP_AUDIO_ERR_OK) ?
            ESP_ERR_INVALID_SIZE : opus_error_to_esp(codec_ret);
    }

    ESP_LOGI(TAG,
             "encoder READY rate=16000 channels=1 frame_ms=60 pcm_bytes=%d max_packet=%d bitrate=%d",
             s_encoder_input_bytes,
             s_encoder_output_bytes,
             VOICE_ASSISTANT_OPUS_BITRATE_BPS);
    return ESP_OK;
}

esp_err_t voice_assistant_opus_encoder_reset(void)
{
    if (s_encoder == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return opus_error_to_esp(esp_opus_enc_reset(s_encoder));
}

esp_err_t voice_assistant_opus_encode(
    const int16_t *pcm,
    size_t sample_count,
    uint8_t *packet,
    size_t packet_capacity,
    size_t *packet_size)
{
    if ((s_encoder == NULL) || (pcm == NULL) || (packet == NULL) ||
        (packet_size == NULL) ||
        (sample_count != VOICE_ASSISTANT_OPUS_PCM_SAMPLES) ||
        (packet_capacity < (size_t)s_encoder_output_bytes) ||
        (packet_capacity > UINT32_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_audio_enc_in_frame_t input = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)VOICE_ASSISTANT_OPUS_PCM_BYTES,
    };
    esp_audio_enc_out_frame_t output = {
        .buffer = packet,
        .len = (uint32_t)packet_capacity,
    };
    const esp_audio_err_t codec_ret =
        esp_opus_enc_process(s_encoder, &input, &output);
    if (codec_ret != ESP_AUDIO_ERR_OK) {
        return opus_error_to_esp(codec_ret);
    }
    if ((output.encoded_bytes == 0U) ||
        (output.encoded_bytes > packet_capacity)) {
        return ESP_ERR_INVALID_SIZE;
    }
    *packet_size = output.encoded_bytes;
    return ESP_OK;
}

esp_err_t voice_assistant_opus_decoder_init(void)
{
    if (s_decoder != NULL) {
        return ESP_OK;
    }
    esp_opus_dec_cfg_t config = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false,
    };
    const esp_audio_err_t codec_ret =
        esp_opus_dec_open(&config, sizeof(config), &s_decoder);
    if (codec_ret != ESP_AUDIO_ERR_OK) {
        s_decoder = NULL;
        ESP_LOGE(TAG, "decoder open failed codec_error=%d", (int)codec_ret);
        return opus_error_to_esp(codec_ret);
    }
    ESP_LOGI(TAG, "decoder READY rate=16000 channels=1 frame_ms=60");
    return ESP_OK;
}

esp_err_t voice_assistant_opus_decoder_reset(void)
{
    if (s_decoder == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return opus_error_to_esp(esp_opus_dec_reset(s_decoder));
}

esp_err_t voice_assistant_opus_decode(
    const uint8_t *packet,
    size_t packet_size,
    int16_t *pcm,
    size_t pcm_capacity_samples,
    size_t *pcm_samples)
{
    if ((s_decoder == NULL) || (packet == NULL) || (packet_size == 0U) ||
        (packet_size > UINT32_MAX) || (pcm == NULL) ||
        (pcm_capacity_samples == 0U) ||
        (pcm_capacity_samples > (UINT32_MAX / sizeof(int16_t))) ||
        (pcm_samples == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_audio_dec_in_raw_t input = {
        .buffer = (uint8_t *)packet,
        .len = (uint32_t)packet_size,
    };
    esp_audio_dec_out_frame_t output = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)(pcm_capacity_samples * sizeof(int16_t)),
    };
    esp_audio_dec_info_t info = {0};
    const esp_audio_err_t codec_ret =
        esp_opus_dec_decode(s_decoder, &input, &output, &info);
    if (codec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG,
                 "decoder rejected packet codec_error=%d packet_bytes=%u",
                 (int)codec_ret,
                 (unsigned)packet_size);
        return opus_error_to_esp(codec_ret);
    }
    if ((input.consumed != packet_size) || (output.decoded_size == 0U) ||
        ((output.decoded_size % sizeof(int16_t)) != 0U) ||
        (output.decoded_size > output.len) ||
        (info.sample_rate != ESP_AUDIO_SAMPLE_RATE_16K) ||
        (info.channel != ESP_AUDIO_MONO) ||
        (info.bits_per_sample != ESP_AUDIO_BIT16)) {
        ESP_LOGE(TAG,
                 "decoder contract invalid consumed=%u/%u decoded=%u rate=%u channels=%u bits=%u",
                 (unsigned)input.consumed,
                 (unsigned)packet_size,
                 (unsigned)output.decoded_size,
                 (unsigned)info.sample_rate,
                 (unsigned)info.channel,
                 (unsigned)info.bits_per_sample);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *pcm_samples = output.decoded_size / sizeof(int16_t);
    return ESP_OK;
}
