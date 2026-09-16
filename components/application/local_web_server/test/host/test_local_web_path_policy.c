#include <stdio.h>
#include <string.h>

#include "local_web_path_policy.h"

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
    failures += expect_rejected("/foo/%2e%2e/bar");
    failures += expect_rejected("/foo/%ZZ");
    failures += expect_rejected("/foo/%");
    failures += expect_rejected("/foo\\bar");

    char long_path[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 2U];
    memset(long_path, 'a', sizeof(long_path) - 1U);
    long_path[sizeof(long_path) - 1U] = '\0';
    failures += expect_rejected(long_path);
    return failures == 0 ? 0 : 1;
}
