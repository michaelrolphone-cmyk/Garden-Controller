#pragma once
/* input.button@1. Generic contact input. No menu or zone policy.
 * A board profile supplies pins, active level, debounce, and long-press time. */
#include <stdbool.h>
#include <stdint.h>
#define BUTTON_API_V1 1u
#define BUTTON_CHANNELS_MAX 8u

typedef struct {
    uint8_t channel_count;
    uint8_t pins[BUTTON_CHANNELS_MAX];
    bool active_low;
    uint32_t debounce_us;
    uint32_t long_press_us;
    uint32_t click_min_us;
} button_profile_t;

typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    uint8_t (*channel_count)(void *context);
    void (*poll)(void *context, uint32_t now_us);
    bool (*is_down)(void *context, uint8_t channel);
    bool (*take_click)(void *context, uint8_t channel);
    bool (*take_long_press)(void *context, uint8_t channel);
} button_api_v1;
