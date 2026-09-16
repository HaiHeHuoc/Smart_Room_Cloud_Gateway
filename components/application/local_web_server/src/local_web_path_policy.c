#include "local_web_path_policy.h"

#include <stdbool.h>
#include <string.h>

static int local_web_hex_value(char character)
{
    if ((character >= '0') && (character <= '9')) return character - '0';
    if ((character >= 'a') && (character <= 'f')) return character - 'a' + 10;
    if ((character >= 'A') && (character <= 'F')) return character - 'A' + 10;
    return -1;
}

esp_err_t local_web_path_policy_normalize(
    const char *encoded_path,
    char *logical_path,
    size_t logical_path_size)
{
    if ((encoded_path == NULL) || (logical_path == NULL) ||
        (logical_path_size < 2U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t input_length = strnlen(
        encoded_path, LOCAL_WEB_LOGICAL_PATH_MAX_LEN * 3U + 1U);
    if ((input_length == 0U) ||
        (input_length > LOCAL_WEB_LOGICAL_PATH_MAX_LEN * 3U))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    char decoded[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    size_t decoded_length = 0U;
    for (size_t input_index = 0U; input_index < input_length; input_index++)
    {
        unsigned char character = (unsigned char)encoded_path[input_index];
        if (character == '%')
        {
            if ((input_index + 2U >= input_length)) return ESP_ERR_INVALID_ARG;
            const int high = local_web_hex_value(encoded_path[++input_index]);
            const int low = local_web_hex_value(encoded_path[++input_index]);
            if ((high < 0) || (low < 0)) return ESP_ERR_INVALID_ARG;
            character = (unsigned char)((high << 4) | low);
        }

        if ((character < 0x20U) || (character == '\\') ||
            (decoded_length >= LOCAL_WEB_LOGICAL_PATH_MAX_LEN))
        {
            return ESP_ERR_INVALID_ARG;
        }
        decoded[decoded_length++] = (char)character;
    }

    if ((decoded_length > 1U) && (decoded[decoded_length - 1U] == '/'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t output_length = 0U;
    logical_path[output_length++] = '/';

    for (size_t index = 0U; index < decoded_length; )
    {
        const size_t segment_start = index;
        while ((index < decoded_length) && (decoded[index] != '/')) index++;
        const size_t segment_length = index - segment_start;
        const bool at_separator = (index < decoded_length);

        if (segment_length == 0U)
        {
            if ((segment_start == 0U) && at_separator)
            {
                index++;
                continue;
            }
            return ESP_ERR_INVALID_ARG;
        }
        if (((segment_length == 1U) && (decoded[segment_start] == '.')) ||
            ((segment_length == 2U) && (decoded[segment_start] == '.') &&
             (decoded[segment_start + 1U] == '.')))
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (segment_length > LOCAL_WEB_LOGICAL_COMPONENT_MAX_LEN)
        {
            return ESP_ERR_INVALID_SIZE;
        }

        if ((output_length > 1U) &&
            (logical_path[output_length - 1U] != '/'))
        {
            if (output_length >= logical_path_size - 1U) return ESP_ERR_INVALID_SIZE;
            logical_path[output_length++] = '/';
        }
        if ((output_length + segment_length) >= logical_path_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(&logical_path[output_length], &decoded[segment_start], segment_length);
        output_length += segment_length;

        if (at_separator) index++;
    }

    logical_path[output_length] = '\0';
    return ESP_OK;
}
