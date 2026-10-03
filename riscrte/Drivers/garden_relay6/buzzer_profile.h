#pragma once
/* Castle Hills buzzer profile. Field firmware chirps GPIO 21 at 5% duty. */
#include "../buzzer/BuzzerApi.h"
static inline buzzer_profile_t buzzer_profile_castle_hills(void) {
    buzzer_profile_t profile = {
        .channel_count = 1,
        .pins = {21},
        .active_high = true
    };
    return profile;
}
