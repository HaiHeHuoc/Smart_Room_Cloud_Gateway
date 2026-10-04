#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "xiaozhi_activation_policy.h"

static bool activation_is_cleared(
    const xiaozhi_foundation_activation_status_t *status)
{
    return (status != NULL) && !status->required && !status->code_available &&
           !status->message_available && (status->code[0] == '\0') &&
           (status->message[0] == '\0') &&
           (status->backend_timeout_ms == 0U);
}

int main(void)
{
    bool ok = true;
    xiaozhi_foundation_activation_status_t status = {0};
    const xiaozhi_activation_upstream_info_t active = {
        .has_activation_code = true,
        .activation_code = "ABC-123",
        .activation_message = "Activate this device in Xiaozhi.",
        .activation_timeout_ms = 1,
    };

    /* Feature disabled deliberately retains the previous session behavior. */
    ok &= !xiaozhi_activation_status_from_upstream(false, &active, &status);
    ok &= activation_is_cleared(&status);

    ok &= xiaozhi_activation_status_from_upstream(true, &active, &status);
    ok &= status.required && status.code_available &&
          (strcmp(status.code, "ABC-123") == 0) &&
          status.message_available && (status.backend_timeout_ms == 1U);

    char long_code[160] = {0};
    char long_message[300] = {0};
    memset(long_code, 'C', sizeof(long_code) - 1U);
    memset(long_message, 'M', sizeof(long_message) - 1U);
    const xiaozhi_activation_upstream_info_t oversized = {
        .has_activation_code = true,
        .activation_code = long_code,
        .activation_message = long_message,
        .activation_timeout_ms = 999999,
    };
    ok &= xiaozhi_activation_status_from_upstream(true, &oversized, &status);
    ok &= status.code_truncated && status.message_truncated &&
          (strlen(status.code) ==
           (XIAOZHI_FOUNDATION_ACTIVATION_CODE_MAX_BYTES - 1U)) &&
          (strlen(status.message) ==
           (XIAOZHI_FOUNDATION_ACTIVATION_MESSAGE_MAX_BYTES - 1U));

    const xiaozhi_activation_upstream_info_t activated = {
        .has_activation_code = false,
    };
    ok &= !xiaozhi_activation_status_from_upstream(true, &activated, &status);
    ok &= activation_is_cleared(&status);

    /* Backend timeout is informational only: retry cadence is project-owned. */
    ok &= (xiaozhi_foundation_activation_retry_delay_ms(1U) == 10000U);
    ok &= (xiaozhi_foundation_activation_retry_delay_ms(2U) == 20000U);
    ok &= (xiaozhi_foundation_activation_retry_delay_ms(3U) == 40000U);
    ok &= (xiaozhi_foundation_activation_retry_delay_ms(4U) == 60000U);
    ok &= (xiaozhi_foundation_activation_retry_delay_ms(100U) == 60000U);

    puts(ok ? "Xiaozhi first-boot activation policy: PASS" :
              "Xiaozhi first-boot activation policy: FAIL");
    return ok ? 0 : 1;
}
