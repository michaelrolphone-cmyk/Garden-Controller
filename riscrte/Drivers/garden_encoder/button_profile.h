#pragma once
/* CrowPanel knob button. Field sketch: GPIO 41, active low, pull-up.
 * The quadrature driver no longer claims this pin. */
#include "../button/ButtonApi.h"
static inline button_profile_t button_profile_crowpanel(void) {
    button_profile_t profile = {
        .channel_count = 1,
        .pins = {41},
        .active_low = true,
        .debounce_us = 30000,
        .long_press_us = 1500000,
        .click_min_us = 30000
    };
    return profile;
}
