#include "PanelApi.h"
#include "../../sdk/RiscProviderV2.h"
typedef struct { bool (*transfer)(uint8_t cs, const uint8_t *bytes, uint16_t len, bool data); void (*write)(uint8_t pin, bool level); } panel_port_t;
static bool running, have_profile, have_port;
static panel_profile_t profile;
static panel_port_t port;
static uint16_t pixel;
static uint32_t dirty;
bool panel_bind_port(const panel_port_t *next) { if (running || !next || !next->transfer || !next->write) return false; port = *next; have_port = true; return true; }
bool panel_bind_profile(const panel_profile_t *next) { if (running || !next || !next->width || !next->height) return false; profile = *next; have_profile = true; return true; }
static uint16_t width(void *c) { (void)c; return running ? profile.width : 0; }
static uint16_t height(void *c) { (void)c; return running ? profile.height : 0; }
static bool set_pixel(void *c, uint16_t x, uint16_t y, uint16_t rgb565) { (void)c; if (!running || x >= profile.width || y >= profile.height) return false; pixel = rgb565; dirty++; return true; }
static bool fill(void *c, uint16_t rgb565) { (void)c; if (!running) return false; pixel = rgb565; dirty = (uint32_t)profile.width * profile.height; return true; }
static bool flush(void *c) { (void)c; if (!running || !dirty) return false; uint8_t bytes[2] = { (uint8_t)(pixel >> 8), (uint8_t)pixel }; bool ok = port.transfer(profile.cs, bytes, 2, true); if (ok) dirty = 0; return ok; }
static bool set_backlight(void *c, uint8_t percent) { (void)c; if (!running || percent > 100) return false; port.write(profile.backlight, percent > 0); return true; }
static const panel_api_v1 api = { PANEL_API_V1, sizeof(panel_api_v1), NULL, width, height, set_pixel, fill, flush, set_backlight };
static bool start(const risc_provider_dependency_v1 *d, size_t n) { (void)d; if (running || n || !have_profile || !have_port) return false; running = true; dirty = 0; return true; }
static bool quiesce(void) { if (running) port.write(profile.backlight, false); return true; }
static void stop(void) { (void)quiesce(); running = false; }
static const risc_driver_v2 driver = { RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2), "panel", "display.output", PANEL_API_V1, &api, start, stop, quiesce };
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL; }
