#include "PixelApi.h"
#include "../../sdk/RiscProviderV2.h"
typedef struct { bool (*write_bits)(uint8_t pin, const uint8_t *rgb, uint8_t count); } pixel_port_t;
static bool running, have_profile, have_port;
static pixel_profile_t profile;
static pixel_port_t port;
static uint8_t frame[PIXEL_CHANNELS_MAX * 3];
static uint8_t brightness = 100;
bool pixel_bind_port(const pixel_port_t *next) { if (running || !next || !next->write_bits) return false; port = *next; have_port = true; return true; }
bool pixel_bind_profile(const pixel_profile_t *next) { if (running || !next || !next->count || next->count > PIXEL_CHANNELS_MAX) return false; profile = *next; have_profile = true; return true; }
static uint8_t count(void *c) { (void)c; return running ? profile.count : 0; }
static bool set_rgb(void *c, uint8_t index, uint8_t r, uint8_t g, uint8_t b) { (void)c; if (!running || index >= profile.count) return false; frame[index * 3] = g; frame[index * 3 + 1] = r; frame[index * 3 + 2] = b; return true; }
static bool set_brightness(void *c, uint8_t percent) { (void)c; if (!running || percent > 100) return false; brightness = percent; return true; }
static bool show(void *c) { (void)c; if (!running) return false; uint8_t scaled[PIXEL_CHANNELS_MAX * 3]; for (uint8_t i = 0; i < profile.count * 3; i++) scaled[i] = (uint8_t)((frame[i] * brightness) / 100); return port.write_bits(profile.pin, scaled, profile.count); }
static const pixel_api_v1 api = { PIXEL_API_V1, sizeof(pixel_api_v1), NULL, count, set_rgb, set_brightness, show };
static bool start(const risc_provider_dependency_v1 *d, size_t n) { (void)d; if (running || n || !have_profile || !have_port) return false; running = true; return true; }
static bool quiesce(void) { if (!running) return true; for (uint8_t i = 0; i < profile.count * 3; i++) frame[i] = 0; return port.write_bits(profile.pin, frame, profile.count); }
static void stop(void) { (void)quiesce(); running = false; }
static const risc_driver_v2 driver = { RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2), "pixel", "indicator.pixel", PIXEL_API_V1, &api, start, stop, quiesce };
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL; }
