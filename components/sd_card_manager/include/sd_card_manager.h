#pragma once

#include <stdbool.h>
#include <esp_err.h>

esp_err_t sd_card_manager_init(void);

esp_err_t sd_card_manager_deinit(void);

bool sd_card_manager_is_mounted(void);

esp_err_t sd_card_manager_write_test_file(void);

esp_err_t sd_card_manager_read_test_file(void);