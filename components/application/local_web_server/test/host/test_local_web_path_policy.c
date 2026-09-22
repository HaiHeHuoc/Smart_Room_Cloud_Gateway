#include <stdio.h>
#include <string.h>

#include "local_web_path_policy.h"
#include "local_web_download.h"

static int expect_content_disposition(const char *path, const char *expected)
{
    char output[LOCAL_WEB_DOWNLOAD_CONTENT_DISPOSITION_MAX_LEN] = {0};
    if (!local_web_download_content_disposition(path, output, sizeof(output)) ||
        (strcmp(output, expected) != 0))
    {
        fprintf(stderr, "unexpected Content-Disposition for %s\n", path);
        return 1;
    }
    return 0;
}

static int expect_ok(const char *input, const char *expected)
{
    char output[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if ((local_web_path_policy_normalize(input, output, sizeof(output)) != ESP_OK) ||
        (strcmp(output, expected) != 0))
    {
        fprintf(stderr, "expected normalized path %s\n", expected);
        return 1;
    }
    return 0;
}

static int expect_rejected(const char *input)
{
    char output[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if (local_web_path_policy_normalize(input, output, sizeof(output)) == ESP_OK)
    {
        fprintf(stderr, "unsafe path accepted\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += expect_ok("/", "/");
    failures += expect_ok("audio", "/audio");
    failures += expect_ok("nested/child", "/nested/child");
    failures += expect_ok("/nested/child", "/nested/child");
    failures += expect_ok("%2Faudio%2Fsample.wav", "/audio/sample.wav");

    failures += expect_rejected("..");
    failures += expect_rejected("../file");
    failures += expect_rejected("foo/../bar");
    failures += expect_rejected("/foo//bar");
    failures += expect_rejected("/foo/");
    failures += expect_rejected("/foo/%2e%2e/bar");
    failures += expect_rejected("/foo/%2Fbar");
    failures += expect_rejected("/foo/%5cbar");
    failures += expect_rejected("/foo/%ZZ");
    failures += expect_rejected("/foo/%");
    failures += expect_rejected("/foo\\bar");

    char long_path[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 2U];
    memset(long_path, 'a', sizeof(long_path) - 1U);
    long_path[sizeof(long_path) - 1U] = '\0';
    failures += expect_rejected(long_path);

    char long_component[LOCAL_WEB_LOGICAL_COMPONENT_MAX_LEN + 2U];
    memset(long_component, 'a', sizeof(long_component) - 1U);
    long_component[sizeof(long_component) - 1U] = '\0';
    failures += expect_rejected(long_component);

    failures += expect_content_disposition(
        "/audio/input_long_3.wav",
        "attachment; filename=\"input_long_3.wav\"; "
        "filename*=UTF-8''input_long_3.wav");
    failures += expect_content_disposition(
        "/audio/report 1.wav",
        "attachment; filename=\"report_1.wav\"; "
        "filename*=UTF-8''report%201.wav");
    failures += local_web_download_content_disposition(
        "/", long_path, sizeof(long_path));
    return failures == 0 ? 0 : 1;
}
