#pragma once
/* input.touch@1. Generic pointer. Controller and bus come from a profile. */
#include <stdbool.h>
#include <stdint.h>
#define TOUCH_API_V1 1u
typedef struct {
    uint8_t sda, scl, addr;
    int8_t irq, reset;
    uint16_t width, height;
} touch_profile_t;
#include "RiscTouchV1.h"
typedef risc_touch_api_v1 touch_api_v1;
