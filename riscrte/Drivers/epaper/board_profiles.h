#pragma once
#include "EpaperApi.h"
static inline epaper_profile_t epaper_profile_gdey075(void) {
    epaper_profile_t profile = {
        .width = 800, .height = 480,
        .mosi = 4, .sclk = 5, .cs = 7, .dc = 3, .rst = 6, .busy = 2
    };
    return profile;
}
