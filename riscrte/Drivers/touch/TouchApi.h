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
typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    void (*poll)(void *context);
    uint8_t (*point_count)(void *context);
    bool (*point)(void *context, uint8_t index, uint16_t *x, uint16_t *y);
} touch_api_v1;
