#pragma once
/* display.panel@1. Generic raster panel. Controller and pins come from a profile. */
#include <stdbool.h>
#include <stdint.h>
#define PANEL_API_V1 1u
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t sclk, mosi, dc, cs, rst, backlight;
    int8_t power_rails[2];
} panel_profile_t;
typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    uint16_t (*width)(void *context);
    uint16_t (*height)(void *context);
    bool (*set_pixel)(void *context, uint16_t x, uint16_t y, uint16_t rgb565);
    bool (*fill)(void *context, uint16_t rgb565);
    bool (*flush)(void *context);
    bool (*set_backlight)(void *context, uint8_t percent);
} panel_api_v1;
