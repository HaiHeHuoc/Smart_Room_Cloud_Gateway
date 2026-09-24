#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    LIGHT_MANAGER_EFFECT_SOLID = 0,
    LIGHT_MANAGER_EFFECT_BLINK,
    LIGHT_MANAGER_EFFECT_BREATH,
    LIGHT_MANAGER_EFFECT_PULSE,
    LIGHT_MANAGER_EFFECT_RAINBOW,
    LIGHT_MANAGER_EFFECT_STROBE,
    LIGHT_MANAGER_EFFECT_HEARTBEAT,
    LIGHT_MANAGER_EFFECT_CANDLE,
    LIGHT_MANAGER_EFFECT_SOS,
    LIGHT_MANAGER_EFFECT_LIGHTNING,
    LIGHT_MANAGER_EFFECT_WAKE_UP,
    LIGHT_MANAGER_EFFECT_SLEEP_FADE,
    LIGHT_MANAGER_EFFECT_NOTIFICATION,
} light_manager_effect_t;

typedef struct
{
    bool power_on;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t brightness_percent;
    light_manager_effect_t effect;
} light_manager_state_t;
