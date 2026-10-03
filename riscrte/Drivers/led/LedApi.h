#pragma once
/* indicator.led@1. Generic LED bank. No alarm, zone, or status policy.
 * A board profile supplies channel count, pins, and active level. */
#include <stdbool.h>
#include <stdint.h>
#define LED_API_V1 1u
#define LED_CHANNELS_MAX 8u

typedef struct {
    uint8_t channel_count;
    uint8_t pins[LED_CHANNELS_MAX];
    bool active_high;
} led_profile_t;

typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    uint8_t (*channel_count)(void *context);
    bool (*set)(void *context, uint8_t channel, bool on);
    bool (*set_duty)(void *context, uint8_t channel, uint8_t percent);
    bool (*blink)(void *context, uint8_t channel, uint16_t on_us, uint16_t off_us, uint8_t count);
    uint8_t (*get_mask)(void *context);
} led_api_v1;
