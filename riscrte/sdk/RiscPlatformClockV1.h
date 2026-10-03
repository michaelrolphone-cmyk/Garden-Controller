#pragma once
/* Generic OS/CPU port service, independent of peripheral identity. Callers
 * obtain it as a verified capability dependency, not as a raw firmware symbol.
 * Duration is monotonic and sleep MUST yield to the scheduler. */
#include "RiscProviderV2.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RISC_PLATFORM_CLOCK_API_V1 1u
typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    uint64_t (*monotonic_ms)(void *context);
    void (*sleep_ms)(void *context, uint32_t milliseconds);
} risc_platform_clock_api_v1;
#ifdef __cplusplus
}
#endif
