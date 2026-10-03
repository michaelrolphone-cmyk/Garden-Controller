#pragma once
#include "PanelApi.h"
static inline panel_profile_t panel_profile_crowpanel(void) {
    panel_profile_t profile = {
        .width = 240, .height = 240,
        .sclk = 10, .mosi = 11, .dc = 3, .cs = 9, .rst = 14, .backlight = 46,
        .power_rails = {1, 2}
    };
    return profile;
}
