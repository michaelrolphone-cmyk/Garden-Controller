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
#include "RiscDisplayOutputV1.h"
typedef risc_display_output_api_v1 panel_api_v1;
