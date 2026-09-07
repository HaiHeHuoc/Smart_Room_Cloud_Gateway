#include "platform.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#define fwrite host_fwrite
#define fsync host_fsync
#define mkdir host_mkdir
#include "../../log_manager.c"
int main(void)
{
    assert(log_manager_init() == ESP_ERR_NOT_SUPPORTED);
    assert(log_manager_start() == ESP_ERR_INVALID_STATE);
    APP_LOGI("TEST", CONSOLE_FALLBACK, "enabled=0");
    assert(host_console == 1 && host_allocations == 0 && host_tasks == 0);
    assert(log_manager_deinit() == ESP_OK);
    puts("PASS disabled configuration, console fallback, zero backend allocation");
    return 0;
}
