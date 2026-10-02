/* switch.relay@1. start() claims the Castle Hills pins if no profile was bound.
 * The loader does not call a bind function. */
#include "RelayApi.h"
#include "../../sdk/RiscProviderV2.h"
typedef struct { bool (*claim_output)(uint8_t pin); void (*write)(uint8_t pin, bool level); void (*release)(uint8_t pin); void (*delay_us)(uint32_t us); } relay_gpio_port_t;
static bool running, have_profile, have_port; static uint8_t mask; static relay_profile_t profile; static relay_gpio_port_t port;
#if defined(GARDEN_RTE_TARGET_ESP32S3)
#include "driver/gpio.h"
static bool esp_claim(uint8_t pin) { gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE }; return gpio_config(&cfg) == ESP_OK; }
static void esp_write(uint8_t pin, bool level) { gpio_set_level((gpio_num_t)pin, level ? 1 : 0); }
static void esp_release(uint8_t pin) { gpio_reset_pin((gpio_num_t)pin); }
static void esp_delay_us(uint32_t us) { esp_rom_delay_us(us); }
static const relay_gpio_port_t esp_port = {esp_claim, esp_write, esp_release, esp_delay_us};
#endif
bool relay_bind_port(const relay_gpio_port_t *next) { if (running || !next || !next->claim_output || !next->write || !next->release) return false; port = *next; have_port = true; return true; }
bool relay_bind_profile(const relay_profile_t *next) { if (running || !next || next->channel_count == 0 || next->channel_count > RELAY_CHANNELS_MAX) return false; profile = *next; have_profile = true; return true; }
static void write_channel(uint8_t index, bool closed) { port.write(profile.pins[index], profile.active_high ? closed : !closed); }
static void apply_mask(uint8_t next) { for (uint8_t i = 0; i < profile.channel_count; i++) write_channel(i, (next & (1u << i)) != 0); mask = next; }
static uint8_t channel_count(void *c) { (void)c; return running ? profile.channel_count : 0; }
static bool set_channel(void *c, uint8_t channel, bool closed) { (void)c; if (!running || channel >= profile.channel_count) return false; if (closed) mask |= (uint8_t)(1u << channel); else mask &= (uint8_t)~(1u << channel); write_channel(channel, closed); return true; }
static bool set_mask(void *c, uint8_t next) { (void)c; if (!running) return false; uint8_t allowed = profile.channel_count == 8 ? 0xffu : (uint8_t)((1u << profile.channel_count) - 1u); apply_mask(next & allowed); return true; }
static uint8_t get_mask(void *c) { (void)c; return running ? mask : 0; }
static bool chirp(void *c) { (void)c; return false; }
static const relay_api_v1 api = { RELAY_API_V1, sizeof(relay_api_v1), NULL, channel_count, set_channel, set_mask, get_mask, chirp };
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    (void)deps; (void)count; if (running) return false;
    if (!have_profile) { relay_profile_t field = { .channel_count = 6, .pins = {1, 2, 41, 42, 45, 46}, .active_high = true, .indicator_pin = -1 }; if (!relay_bind_profile(&field)) return false; }
#if defined(GARDEN_RTE_TARGET_ESP32S3)
    if (!have_port) { port = esp_port; have_port = true; }
#endif
    if (!have_port) return false;
    for (uint8_t i = 0; i < profile.channel_count; i++) { if (!port.claim_output(profile.pins[i])) { for (uint8_t j = 0; j < i; j++) { write_channel(j, false); port.release(profile.pins[j]); } return false; } write_channel(i, false); }
    mask = 0; running = true; return true;
}
static bool quiesce(void) { if (!running) return true; if (port.write) for (uint8_t i = 0; i < profile.channel_count; i++) write_channel(i, false); mask = 0; return true; }
static void stop(void) { (void)quiesce(); if (port.release) for (uint8_t i = 0; i < profile.channel_count; i++) port.release(profile.pins[i]); running = false; }
static const risc_driver_v2 driver = { RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2), "relay", "switch.relay", RELAY_API_V1, &api, start, stop, quiesce };
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL; }
