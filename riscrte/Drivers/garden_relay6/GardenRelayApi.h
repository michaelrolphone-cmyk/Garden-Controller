#pragma once
/* switch.relay@1. Generic contact bank. The driver does not know what a
 * channel switches. Callers own policy: irrigation, lights, pumps, locks.
 * This board profile, from mcu/relay/GardenSimpleRelay6Core.inc:
 *   channels 1..6 on GPIO 1, 2, 41, 42, 45, 46, active HIGH
 *   optional indicator on GPIO 21, 5% duty chirp
 * Quiesce and stop force every output off. */
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
