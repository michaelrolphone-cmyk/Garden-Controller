/* Generic RiscRTE relay driver. Publishes switch.relay@1.
 * Does not know what a channel switches. Pins come from a bound profile.
 * Quiesce and stop open every contact. */
#include "RelayApi.h"
#include "../../sdk/RiscProviderV2.h"

typedef struct {
    bool (*claim_output)(uint8_t pin);
    void (*write)(uint8_t pin, bool level);
    void (*release)(uint8_t pin);
    void (*delay_us)(uint32_t us);
} relay_gpio_port_t;

static bool running;
static uint8_t mask;
static relay_profile_t profile;
static relay_gpio_port_t port;
static bool have_profile;
static bool have_port;

#if defined(GARDEN_RTE_TARGET_ESP32S3)
#include "driver/gpio.h"
static bool esp_claim(uint8_t pin) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg) == ESP_OK;
}
static void esp_write(uint8_t pin, bool level) { gpio_set_level((gpio_num_t)pin, level ? 1 : 0); }
static void esp_release(uint8_t pin) { gpio_reset_pin((gpio_num_t)pin); }
static void esp_delay_us(uint32_t us) { esp_rom_delay_us(us); }
static const relay_gpio_port_t esp_port = {esp_claim, esp_write, esp_release, esp_delay_us};
#endif

bool relay_bind_port(const relay_gpio_port_t *next) {
    if (running || !next || !next->claim_output || !next->write || !next->release) return false;
    port = *next;
    have_port = true;
    return true;
}
bool relay_bind_profile(const relay_profile_t *next) {
    if (running || !next || next->channel_count == 0 || next->channel_count > RELAY_CHANNELS_MAX) return false;
    for (uint8_t i = 0; i < next->channel_count; i++) {
        for (uint8_t j = 0; j < i; j++) if (next->pins[i] == next->pins[j]) return false;
        if (next->indicator_pin >= 0 && next->pins[i] == (uint8_t)next->indicator_pin) return false;
    }
    profile = *next;
    have_profile = true;
    return true;
}
static void write_channel(uint8_t index, bool closed) {
    bool level = profile.active_high ? closed : !closed;
    port.write(profile.pins[index], level);
}
static void apply_mask(uint8_t next) {
    for (uint8_t i = 0; i < profile.channel_count; i++) write_channel(i, (next & (1u << i)) != 0);
    mask = next;
}
static uint8_t channel_count(void *context) { (void)context; return running ? profile.channel_count : 0; }
static bool set_channel(void *context, uint8_t channel, bool closed) {
    (void)context;
    if (!running || channel >= profile.channel_count) return false;
    if (closed) mask |= (uint8_t)(1u << channel);
    else mask &= (uint8_t)~(1u << channel);
    write_channel(channel, closed);
    return true;
}
static bool set_mask(void *context, uint8_t next) {
    (void)context;
    if (!running) return false;
    uint8_t allowed = profile.channel_count == 8 ? 0xffu : (uint8_t)((1u << profile.channel_count) - 1u);
    apply_mask(next & allowed);
    return true;
}
static uint8_t get_mask(void *context) { (void)context; return running ? mask : 0; }
static bool chirp(void *context) {
    (void)context;
    if (!running || profile.indicator_pin < 0 || !port.delay_us) return false;
    for (uint8_t n = 0; n < 8; n++) {
        port.write((uint8_t)profile.indicator_pin, true);
        port.delay_us(50);
        port.write((uint8_t)profile.indicator_pin, false);
        port.delay_us(950);
    }
    return true;
}
static const relay_api_v1 api = {
    RELAY_API_V1, sizeof(relay_api_v1), NULL,
    channel_count, set_channel, set_mask, get_mask, chirp
};
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    (void)deps;
    if (running || count || !have_profile) return false;
#if defined(GARDEN_RTE_TARGET_ESP32S3)
    if (!have_port) { port = esp_port; have_port = true; }
#endif
    if (!have_port) return false;
    if (profile.indicator_pin >= 0) {
        if (!port.claim_output((uint8_t)profile.indicator_pin)) return false;
        port.write((uint8_t)profile.indicator_pin, false);
    }
    for (uint8_t i = 0; i < profile.channel_count; i++) {
        if (!port.claim_output(profile.pins[i])) {
            for (uint8_t j = 0; j < i; j++) { write_channel(j, false); port.release(profile.pins[j]); }
            if (profile.indicator_pin >= 0) port.release((uint8_t)profile.indicator_pin);
            return false;
        }
        write_channel(i, false);
    }
    mask = 0;
    running = true;
    return true;
}
static bool quiesce(void) {
    if (!running) return true;
    if (port.write) {
        for (uint8_t i = 0; i < profile.channel_count; i++) write_channel(i, false);
        if (profile.indicator_pin >= 0) port.write((uint8_t)profile.indicator_pin, false);
    }
    mask = 0;
    return true;
}
static void stop(void) {
    (void)quiesce();
    if (port.release) {
        for (uint8_t i = 0; i < profile.channel_count; i++) port.release(profile.pins[i]);
        if (profile.indicator_pin >= 0) port.release((uint8_t)profile.indicator_pin);
    }
    running = false;
}
static const risc_driver_v2 driver = {
    RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2),
    "relay", "switch.relay", RELAY_API_V1,
    &api, start, stop, quiesce
};
__attribute__((visibility("default")))
const risc_driver_v2 *t5_driver_get(uint32_t abi) {
    return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL;
}
