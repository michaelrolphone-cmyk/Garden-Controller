#pragma once
/* display.epaper@1. Generic reflective panel. Size and pins come from a profile. */
#include <stdbool.h>
#include <stdint.h>
#define EPAPER_API_V1 1u
typedef struct { uint16_t width, height; uint8_t mosi, sclk, cs, dc, rst, busy; } epaper_profile_t;
typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    uint16_t (*width)(void *context);
    uint16_t (*height)(void *context);
    bool (*set_pixel)(void *context, uint16_t x, uint16_t y, bool black);
    bool (*refresh)(void *context);
    bool (*busy)(void *context);
} epaper_api_v1;
