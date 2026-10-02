#pragma once
/* Castle Hills indicator profile. Field firmware chirps GPIO 21.
 * The pin is an indicator output; this profile does not decide LED versus buzzer. */
#include "../led/LedApi.h"
static inline led_profile_t led_profile_castle_hills(void) {
    led_profile_t profile = {
        .channel_count = 1,
        .pins = {21},
        .active_high = true
    };
    return profile;
}
