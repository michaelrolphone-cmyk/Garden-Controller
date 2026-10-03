#pragma once
/* Castle Hills 6-channel board profile for the generic relay driver.
 * Pins from mcu/relay/GardenSimpleRelay6Core.inc. GPIO 21 is the buzzer; GPIO 38 is an addressable pixel,
 * neither is a relay or plain LED. */
#include "../relay/RelayApi.h"
static inline relay_profile_t relay_profile_castle_hills(void) {
    relay_profile_t profile = {
        .channel_count = 6,
        .pins = {1, 2, 41, 42, 45, 46},
        .active_high = true,
        .indicator_pin = -1
    };
    return profile;
}
