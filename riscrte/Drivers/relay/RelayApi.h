#pragma once
/* switch.relay@1. Generic contact bank. No zone, valve, or load policy.
 * A board profile supplies channel count, pins, and active level. */
#include <stdbool.h>
#include <stdint.h>
#define RELAY_API_V1 1u
#define RELAY_CHANNELS_MAX 8u

typedef struct {
    uint8_t channel_count;
    uint8_t pins[RELAY_CHANNELS_MAX];
    bool active_high;
    int8_t indicator_pin; /* -1 if this board has no indicator */
} relay_profile_t;

typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    uint8_t (*channel_count)(void *context);
    bool (*set_channel)(void *context, uint8_t channel, bool closed);
    bool (*set_mask)(void *context, uint8_t mask);
    uint8_t (*get_mask)(void *context);
    bool (*chirp)(void *context);
} relay_api_v1;
