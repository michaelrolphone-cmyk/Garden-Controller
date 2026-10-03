#pragma once
/* indicator.pixel@1. Generic addressable pixels. Wire protocol is a provider detail. */
#include <stdbool.h>
#include <stdint.h>
#define PIXEL_API_V1 1u
#define PIXEL_CHANNELS_MAX 16u
typedef struct { uint8_t pin; uint8_t count; } pixel_profile_t;
typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    uint8_t (*count)(void *context);
    bool (*set_rgb)(void *context, uint8_t index, uint8_t r, uint8_t g, uint8_t b);
    bool (*set_brightness)(void *context, uint8_t percent);
    bool (*show)(void *context);
} pixel_api_v1;
