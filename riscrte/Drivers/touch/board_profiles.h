#pragma once
#include "TouchApi.h"
static inline touch_profile_t touch_profile_crowpanel(void) {
    touch_profile_t profile = {
        .sda = 6, .scl = 7, .addr = 0x15, .irq = 5, .reset = 13,
        .width = 240, .height = 240
    };
    return profile;
}
