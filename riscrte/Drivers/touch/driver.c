#include "TouchApi.h"
#include "../../sdk/RiscProviderV2.h"
typedef struct { bool (*read)(uint8_t addr, uint8_t reg, uint8_t *dst, uint8_t len); } touch_port_t;
static bool running, have_profile, have_port, down;
static touch_profile_t profile;
static touch_port_t port;
static uint16_t px, py;
bool touch_bind_port(const touch_port_t *next) { if (running || !next || !next->read) return false; port = *next; have_port = true; return true; }
bool touch_bind_profile(const touch_profile_t *next) { if (running || !next || !next->width || !next->height) return false; profile = *next; have_profile = true; return true; }
static void poll(void *c) { (void)c; if (!running) return; uint8_t raw[5] = {0}; if (!port.read(profile.addr, 0x02, raw, 5)) { down = false; return; } down = (raw[0] & 0x0f) != 0; px = (uint16_t)(((raw[1] & 0x0f) << 8) | raw[2]); py = (uint16_t)(((raw[3] & 0x0f) << 8) | raw[4]); if (px >= profile.width) px = profile.width - 1; if (py >= profile.height) py = profile.height - 1; }
static uint8_t point_count(void *c) { (void)c; return running && down ? 1 : 0; }
static bool point(void *c, uint8_t index, uint16_t *x, uint16_t *y) { (void)c; if (!running || !down || index || !x || !y) return false; *x = px; *y = py; return true; }
static const touch_api_v1 api = { TOUCH_API_V1, sizeof(touch_api_v1), NULL, poll, point_count, point };
static bool start(const risc_provider_dependency_v1 *d, size_t n) { (void)d; if (running || n < 2 || !have_profile || !have_port) return false; running = true; down = false; return true; }
static bool quiesce(void) { down = false; return true; }
static void stop(void) { running = false; down = false; }
static const risc_driver_v2 driver = { RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2), "touch", "input.touch.raw", TOUCH_API_V1, &api, start, stop, quiesce };
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL; }
