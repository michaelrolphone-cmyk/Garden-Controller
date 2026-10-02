#pragma once
/* irrigation.relay@1. Pins are owned by this driver, not by the app.
 * Castle Hills relay board, from mcu/relay/GardenSimpleRelay6Core.inc:
 *   relays 1..6 on GPIO 1, 2, 41, 42, 45, 46, active HIGH
 *   passive buzzer on GPIO 21, 5% duty chirp
 * Channel 6 is the master valve. The app decides when it is on; this driver
 * only drives the requested mask and forces every output off on stop. */
#include <stdbool.h>
#include <stdint.h>
#define GARDEN_RELAY_API_V1 1u
#define GARDEN_RELAY_CHANNELS 6u

typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    bool (*set_mask)(void *context, uint8_t mask);
    uint8_t (*get_mask)(void *context);
    bool (*chirp)(void *context);
} garden_relay_api_v1;
