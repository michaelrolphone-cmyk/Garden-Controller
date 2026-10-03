#pragma once
/* CrowPanel POWER_LIGHT_PIN is a plain LED. Relay GPIO38 and dial GPIO48
 * are WS2812 pixels; use pixel/board_profiles.h for those. */
#include "../led/LedApi.h"
static inline led_profile_t led_profile_crowpanel(void) {
    return (led_profile_t){.channel_count=1,.pins={40},.active_high=true};
}
