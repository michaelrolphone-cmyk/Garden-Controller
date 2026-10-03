#pragma once
#include "PixelApi.h"
static inline pixel_profile_t pixel_profile_crowpanel(void) {
    pixel_profile_t profile = { .pin = 48, .count = 5 };
    return profile;
}
static inline pixel_profile_t pixel_profile_relay_status(void) {
    pixel_profile_t profile = { .pin = 38, .count = 1 };
    return profile;
}
