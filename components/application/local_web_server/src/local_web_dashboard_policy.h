#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_manager.h"
#include "cloud_manager.h"
#include "sd_card_manager.h"
#include "sensor_manager.h"
#include "time_manager.h"
#include "wifi_manager.h"

typedef enum
{
    LOCAL_WEB_DASHBOARD_HEALTH_NORMAL = 0,
    LOCAL_WEB_DASHBOARD_HEALTH_ATTENTION,
    LOCAL_WEB_DASHBOARD_HEALTH_UNAVAILABLE,
} local_web_dashboard_health_t;

const char *local_web_dashboard_sensor_state_name(sensor_manager_state_t state);
const char *local_web_dashboard_storage_state_name(sd_card_manager_state_t state);
const char *local_web_dashboard_audio_state_name(audio_manager_state_t state);
const char *local_web_dashboard_cloud_state_name(cloud_manager_state_t state);
const char *local_web_dashboard_network_state_name(wifi_manager_state_t state);
const char *local_web_dashboard_time_state_name(time_manager_state_t state);

bool local_web_dashboard_sensor_has_current_data(
    const sensor_manager_status_t *status);
uint64_t local_web_dashboard_age_ms(uint64_t now_ms, int64_t then_ms);

local_web_dashboard_health_t local_web_dashboard_sensor_health(
    const sensor_manager_status_t *status);
local_web_dashboard_health_t local_web_dashboard_storage_health(
    const sd_card_manager_status_t *status, bool capacity_valid);
local_web_dashboard_health_t local_web_dashboard_audio_health(
    const audio_manager_status_t *status, bool playback_status_available);
local_web_dashboard_health_t local_web_dashboard_cloud_health(
    const cloud_manager_status_t *status);
local_web_dashboard_health_t local_web_dashboard_network_health(
    const wifi_manager_status_t *status);
local_web_dashboard_health_t local_web_dashboard_time_health(
    const time_manager_status_t *status);

const char *local_web_dashboard_overall_state(
    const local_web_dashboard_health_t *health,
    size_t health_count);
