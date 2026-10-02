#pragma once
#include "StorageApi.h"
static inline storage_profile_t storage_profile_paper_sd(void) {
    storage_profile_t profile = {
        .cs = 20, .miso = 19, .mosi = 4, .sclk = 5,
        .block_size = 512, .block_count = 0
    };
    return profile;
}
