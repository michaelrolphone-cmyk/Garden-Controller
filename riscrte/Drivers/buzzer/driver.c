/* sound.buzzer@1. start() claims GPIO 21 if no profile was bound. */
#include "BuzzerApi.h"
#include "../../sdk/RiscProviderV2.h"
typedef struct { bool (*claim_output)(uint8_t pin); void (*write)(uint8_t pin, bool level); void (*release)(uint8_t pin); void (*delay_us)(uint32_t us); } buzzer_gpio_port_t;
static bool running, have_profile, have_port; static uint8_t on_mask; static buzzer_profile_t profile; static buzzer_gpio_port_t port;
#if defined(GARDEN_RTE_TARGET_ESP32S3)
#include "driver/gpio.h"
static bool esp_claim(uint8_t pin) { gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE }; return gpio_config(&cfg) == ESP_OK; }
static void esp_write(uint8_t pin, bool level) { gpio_set_level((gpio_num_t)pin, level ? 1 : 0); }
static void esp_release(uint8_t pin) { gpio_reset_pin((gpio_num_t)pin); }
static void esp_delay_us(uint32_t us) { esp_rom_delay_us(us); }
static const buzzer_gpio_port_t esp_port = {esp_claim, esp_write, esp_release, esp_delay_us};
#endif
bool buzzer_bind_port(const buzzer_gpio_port_t *next) { if (running || !next || !next->claim_output || !next->write || !next->release) return false; port = *next; have_port = true; return true; }
bool buzzer_bind_profile(const buzzer_profile_t *next) { if (running || !next || next->channel_count == 0 || next->channel_count > BUZZER_CHANNELS_MAX) return false; profile = *next; have_profile = true; return true; }
static void write_channel(uint8_t index, bool on) { port.write(profile.pins[index], profile.active_high ? on : !on); if (on) on_mask |= (uint8_t)(1u << index); else on_mask &= (uint8_t)~(1u << index); }
static uint8_t channel_count(void *c) { (void)c; return running ? profile.channel_count : 0; }
static bool set(void *c, uint8_t channel, bool on) { (void)c; if (!running || channel >= profile.channel_count) return false; write_channel(channel, on); return true; }
static bool pattern(void *c, uint8_t channel, uint16_t on_us, uint16_t off_us, uint8_t count) { (void)c; if (!running || channel >= profile.channel_count || !count || !port.delay_us) return false; for (uint8_t n = 0; n < count; n++) { write_channel(channel, true); port.delay_us(on_us); write_channel(channel, false); port.delay_us(off_us); } return true; }
static bool chirp(void *c, uint8_t channel) { return pattern(c, channel, 50, 950, 8); }
static bool is_on(void *c, uint8_t channel) { (void)c; return running && channel < profile.channel_count && (on_mask & (1u << channel)) != 0; }
static const buzzer_api_v1 api = { BUZZER_API_V1, sizeof(buzzer_api_v1), NULL, channel_count, set, chirp, pattern, is_on };
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    (void)deps; (void)count; if (running) return false;
    if (!have_profile) { buzzer_profile_t field = { .channel_count = 1, .pins = {21}, .active_high = true }; if (!buzzer_bind_profile(&field)) return false; }
#if defined(GARDEN_RTE_TARGET_ESP32S3)
    if (!have_port) { port = esp_port; have_port = true; }
#endif
    if (!have_port) return false;
    for (uint8_t i = 0; i < profile.channel_count; i++) { if (!port.claim_output(profile.pins[i])) { for (uint8_t j = 0; j < i; j++) { write_channel(j, false); port.release(profile.pins[j]); } return false; } write_channel(i, false); }
    on_mask = 0; running = true; return true;
}
static bool quiesce(void) { if (!running) return true; if (port.write) for (uint8_t i = 0; i < profile.channel_count; i++) write_channel(i, false); on_mask = 0; return true; }
static void stop(void) { (void)quiesce(); if (port.release) for (uint8_t i = 0; i < profile.channel_count; i++) port.release(profile.pins[i]); running = false; }
static const risc_driver_v2 driver = { RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2), "buzzer", "sound.buzzer", BUZZER_API_V1, &api, start, stop, quiesce };
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL; }
