#include "local_web_download.h"

#include <stdio.h>
#include <string.h>

static bool local_web_download_is_attr_char(unsigned char character)
{
    return ((character >= 'a') && (character <= 'z')) ||
           ((character >= 'A') && (character <= 'Z')) ||
           ((character >= '0') && (character <= '9')) ||
           (character == '!') || (character == '#') ||
           (character == '$') || (character == '&') ||
           (character == '+') || (character == '-') ||
           (character == '.') || (character == '^') ||
           (character == '_') || (character == '`') ||
           (character == '|') || (character == '~');
}

bool local_web_download_content_disposition(
    const char *logical_path,
    char *output,
    size_t output_size)
{
    if ((logical_path == NULL) || (output == NULL) || (output_size == 0U))
    {
        return false;
    }

    const char *filename = strrchr(logical_path, '/');
    if ((filename == NULL) || (filename[1] == '\0'))
    {
        return false;
    }

    filename++;
    const size_t filename_length = strnlen(
        filename, LOCAL_WEB_LOGICAL_COMPONENT_MAX_LEN + 1U);
    if ((filename_length == 0U) ||
        (filename_length > LOCAL_WEB_LOGICAL_COMPONENT_MAX_LEN))
    {
        return false;
    }

    char fallback[LOCAL_WEB_LOGICAL_COMPONENT_MAX_LEN + 1U] = {0};
    char encoded[(LOCAL_WEB_LOGICAL_COMPONENT_MAX_LEN * 3U) + 1U] = {0};
    static const char hex[] = "0123456789ABCDEF";
    size_t encoded_length = 0U;

    for (size_t index = 0U; index < filename_length; index++)
    {
        const unsigned char character = (unsigned char)filename[index];
        fallback[index] = local_web_download_is_attr_char(character)
                              ? (char)character
                              : '_';
        if (local_web_download_is_attr_char(character))
        {
            encoded[encoded_length++] = (char)character;
        }
        else
        {
            encoded[encoded_length++] = '%';
            encoded[encoded_length++] = hex[character >> 4U];
            encoded[encoded_length++] = hex[character & 0x0FU];
        }
    }

    const int written = snprintf(
        output, output_size,
        "attachment; filename=\"%s\"; filename*=UTF-8''%s",
        fallback, encoded);
    return (written >= 0) && ((size_t)written < output_size);
}
