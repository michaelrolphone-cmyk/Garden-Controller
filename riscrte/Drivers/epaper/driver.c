#include "EpaperApi.h"
#include "../../sdk/RiscProviderV2.h"
typedef struct { bool (*transfer)(uint8_t cs, const uint8_t *bytes, uint16_t len, bool data); bool (*read_busy)(uint8_t pin); } epaper_port_t;
static bool running, have_profile, have_port, refreshing;
static epaper_profile_t profile;
static epaper_port_t port;
static uint32_t dirty;
bool epaper_bind_port(const epaper_port_t *next) { if (running || !next || !next->transfer || !next->read_busy) return false; port = *next; have_port = true; return true; }
bool epaper_bind_profile(const epaper_profile_t *next) { if (running || !next || !next->width || !next->height) return false; profile = *next; have_profile = true; return true; }
static uint16_t width(void *c) { (void)c; return running ? profile.width : 0; }
static uint16_t height(void *c) { (void)c; return running ? profile.height : 0; }
static bool set_pixel(void *c, uint16_t x, uint16_t y, bool black) { (void)c; (void)black; if (!running || x >= profile.width || y >= profile.height) return false; dirty++; return true; }
static bool refresh(void *c) { (void)c; if (!running || refreshing || port.read_busy(profile.busy)) return false; uint8_t cmd = 0x12; if (!port.transfer(profile.cs, &cmd, 1, false)) return false; refreshing = true; dirty = 0; return true; }
static bool busy(void *c) { (void)c; if (!running) return false; refreshing = port.read_busy(profile.busy); return refreshing; }
static const epaper_api_v1 api = { EPAPER_API_V1, sizeof(epaper_api_v1), NULL, width, height, set_pixel, refresh, busy };
static bool start(const risc_provider_dependency_v1 *d, size_t n) { (void)d; if (running || n || !have_profile || !have_port) return false; running = true; return true; }
static bool quiesce(void) { refreshing = false; return true; }
static void stop(void) { running = false; refreshing = false; }
static const risc_driver_v2 driver = { RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2), "epaper", "display.epaper", EPAPER_API_V1, &api, start, stop, quiesce };
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL; }
