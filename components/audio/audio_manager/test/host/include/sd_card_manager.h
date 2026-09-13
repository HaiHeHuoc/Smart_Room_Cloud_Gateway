#pragma once

#include <stdbool.h>

#include "esp_err.h"

bool sd_card_manager_is_mounted(void);
esp_err_t sd_card_manager_acquire(void);
void sd_card_manager_release(void);
void sd_card_manager_report_io_error(esp_err_t error);
bool sd_card_manager_is_vfs_media_error(int error_number);
