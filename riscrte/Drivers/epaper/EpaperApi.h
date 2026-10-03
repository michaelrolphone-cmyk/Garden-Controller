#pragma once
/* display.epaper@1. Generic reflective panel. Size and pins come from a profile. */
#include <stdbool.h>
#include <stdint.h>
#define EPAPER_API_V1 1u
typedef struct { uint16_t width, height; uint8_t mosi, sclk, cs, dc, rst, busy; } epaper_profile_t;
#include "RiscDisplayOutputV1.h"
typedef risc_display_output_api_v1 epaper_api_v1;
