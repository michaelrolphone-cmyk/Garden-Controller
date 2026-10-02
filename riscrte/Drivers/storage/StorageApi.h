#pragma once
/* storage.block@1. Generic block store. Medium comes from a profile. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define STORAGE_API_V1 1u
typedef struct { uint8_t cs, miso, mosi, sclk; uint32_t block_size; uint32_t block_count; } storage_profile_t;
typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    uint32_t (*block_size)(void *context);
    uint32_t (*block_count)(void *context);
    bool (*read)(void *context, uint32_t block, void *dst, size_t len);
    bool (*write)(void *context, uint32_t block, const void *src, size_t len);
} storage_api_v1;
