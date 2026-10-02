#pragma once
/* Field LED pins. Not the buzzer.
 * Relay board: RGB_LED_PIN 38 in mcu/relay/GardenSimpleRelay6Core.inc.
 * CrowPanel ring: LED_PIN 48 in mcu/dial/GardenKnob.ino. */
#include "../led/LedApi.h"
static inline led_profile_t led_profile_castle_hills(void) {
    led_profile_t profile = {
        .channel_count = 1,
        .pins = {38},
        .active_high = true
    };
    return profile;
}
static inline led_profile_t led_profile_crowpanel(void) {
    led_profile_t profile = {
        .channel_count = 1,
        .pins = {48},
        .active_high = true
    };
    return profile;
}
