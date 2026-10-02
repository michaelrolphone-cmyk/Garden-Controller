#pragma once
/* input.quadrature@1 for the Elecrow CrowPanel 1.28 rotary HMI.
 * Field map from mcu/dial/GardenKnob.ino:
 *   A GPIO 45, B GPIO 42, button GPIO 41, active low, pull-up
 *   2 quadrature edges per tactile detent, 800 us edge debounce
 * Direction matches the field ISR: A==B increments, otherwise decrements. */
#include <stdbool.h>
#include <stdint.h>
#define GARDEN_ENCODER_API_V1 1u

typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    int32_t (*take_detents)(void *context);
    bool (*button_pressed)(void *context);
    bool (*take_click)(void *context);
    bool (*take_long_press)(void *context);
    void (*poll)(void *context, uint32_t now_us);
} garden_encoder_api_v1;
