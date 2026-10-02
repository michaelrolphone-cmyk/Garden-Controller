#pragma once
/* sound.buzzer@1. Generic sounder. No alarm or status policy.
 * A board profile supplies channel count, pins, and active level. */
#include <stdbool.h>
#include <stdint.h>
#define BUZZER_API_V1 1u
#define BUZZER_CHANNELS_MAX 4u

typedef struct {
    uint8_t channel_count;
    uint8_t pins[BUZZER_CHANNELS_MAX];
    bool active_high;
} buzzer_profile_t;

typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    uint8_t (*channel_count)(void *context);
    bool (*set)(void *context, uint8_t channel, bool on);
    bool (*chirp)(void *context, uint8_t channel);
    bool (*pattern)(void *context, uint8_t channel, uint16_t on_us, uint16_t off_us, uint8_t count);
    bool (*is_on)(void *context, uint8_t channel);
} buzzer_api_v1;
