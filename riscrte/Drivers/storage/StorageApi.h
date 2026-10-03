#pragma once
/* storage.block@1. Generic block store. Medium comes from a profile. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define STORAGE_API_V1 1u
typedef struct { uint8_t cs, miso, mosi, sclk; uint32_t block_size; uint32_t block_count; } storage_profile_t;
#include "RiscStorageVolumeV1.h"
typedef risc_storage_volume_api_v1 storage_api_v1;
