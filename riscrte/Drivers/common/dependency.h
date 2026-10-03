#pragma once
#include "GardenPlatformV1.h"
#include <string.h>
static inline const void *garden_dependency(const risc_provider_dependency_v1 *d, size_t n,
                                     const char *name, size_t size) {
    const void *result = NULL;
    if ((!d && n) || n > 16) return NULL;
    for (size_t i = 0; i < n; ++i) {
        if (!d[i].capability_id || strcmp(d[i].capability_id, name)) continue;
        if (result || d[i].api_version != 1 || !d[i].api) return NULL;
        const uint32_t *header = d[i].api;
        if (header[0] != 1 || header[1] < size) return NULL;
        result = d[i].api;
    }
    return result;
}
