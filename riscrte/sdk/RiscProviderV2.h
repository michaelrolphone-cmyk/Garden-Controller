#pragma once
/* Vendored provider root from michaelrolphone-cmyk/T5S3-Reader
 * sdk/driver/RiscProviderV2.h at 7e6509d2 (master). Capability strings stay
 * opaque to the runtime. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RISC_PROVIDER_DRIVER_ABI_V2 2u

typedef struct {
    const char *capability_id;
    uint32_t api_version;
    const void *api;
} risc_provider_dependency_v1;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *driver_id;
    const char *capability_id;
    uint32_t capability_api;
    const void *capability;
    bool (*start)(const risc_provider_dependency_v1 *dependencies, size_t count);
    void (*stop)(void);
    bool (*quiesce)(void);
} risc_driver_v2;

const risc_driver_v2 *t5_driver_get(uint32_t abi);
#ifdef __cplusplus
}
#endif
