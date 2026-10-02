#include "StorageApi.h"
#include "../../sdk/RiscProviderV2.h"
typedef struct { bool (*transfer)(uint8_t cs, const uint8_t *tx, uint8_t *rx, uint16_t len); } storage_port_t;
static bool running, have_profile, have_port;
static storage_profile_t profile;
static storage_port_t port;
bool storage_bind_port(const storage_port_t *next) { if (running || !next || !next->transfer) return false; port = *next; have_port = true; return true; }
bool storage_bind_profile(const storage_profile_t *next) { if (running || !next || !next->block_size || !next->block_count) return false; profile = *next; have_profile = true; return true; }
static uint32_t block_size(void *c) { (void)c; return running ? profile.block_size : 0; }
static uint32_t block_count(void *c) { (void)c; return running ? profile.block_count : 0; }
static bool read(void *c, uint32_t block, void *dst, size_t len) { (void)c; if (!running || !dst || !len || len > profile.block_size || block >= profile.block_count) return false; uint8_t cmd[5] = { 0x51, (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block }; return port.transfer(profile.cs, cmd, dst, (uint16_t)len); }
static bool write(void *c, uint32_t block, const void *src, size_t len) { (void)c; if (!running || !src || !len || len > profile.block_size || block >= profile.block_count) return false; return port.transfer(profile.cs, src, NULL, (uint16_t)len); }
static const storage_api_v1 api = { STORAGE_API_V1, sizeof(storage_api_v1), NULL, block_size, block_count, read, write };
static bool start(const risc_provider_dependency_v1 *d, size_t n) { (void)d; if (running || n || !have_profile || !have_port) return false; running = true; return true; }
static bool quiesce(void) { return true; }
static void stop(void) { running = false; }
static const risc_driver_v2 driver = { RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2), "storage", "storage.block", STORAGE_API_V1, &api, start, stop, quiesce };
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL; }
