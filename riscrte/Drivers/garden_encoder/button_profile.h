#pragma once
/* CrowPanel buttons from mcu/dial/GardenKnob.ino and the Elecrow S3 rotary map.
 * Channel 0 is the display/knob button, ENCODER_BTN 41, active low.
 * Channel 1 is BOOT, GPIO 0, active low. Quadrature does not claim either pin. */
#include "../button/ButtonApi.h"
static inline button_profile_t button_profile_crowpanel(void) {
    button_profile_t profile = {
        .channel_count = 2,
        .pins = {41, 0},
        .active_low = true,
        .debounce_us = 30000,
        .long_press_us = 1500000,
        .click_min_us = 30000
    };
    return profile;
}
