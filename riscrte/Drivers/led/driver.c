/* Generic RiscRTE LED indicator driver. Publishes indicator.led@1.
 * Does not know what a channel means. Pins come from a bound profile.
 * Quiesce and stop force every channel off. */
#include "LedApi.h"
#include "../../sdk/RiscProviderV2.h"

typedef struct {
    bool (*claim_output)(uint8_t pin);
    void (*write)(uint8_t pin, bool level);
    void (*release)(uint8_t pin);
    void (*delay_us)(uint32_t us);
} led_gpio_port_t;

static bool running;
static uint8_t mask;
static uint8_t duty[LED_CHANNELS_MAX];
static led_profile_t profile;
static led_gpio_port_t port;
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
static const led_gpio_port_t esp_port = {esp_claim, esp_write, esp_release, esp_delay_us};
#endif

bool led_bind_port(const led_gpio_port_t *next) {
    if (running || !next || !next->claim_output || !next->write || !next->release) return false;
    port = *next;
    have_port = true;
    return true;
}
bool led_bind_profile(const led_profile_t *next) {
    if (running || !next || next->channel_count == 0 || next->channel_count > LED_CHANNELS_MAX) return false;
    for (uint8_t i = 0; i < next->channel_count; i++) {
        for (uint8_t j = 0; j < i; j++) if (next->pins[i] == next->pins[j]) return false;
    }
    profile = *next;
    have_profile = true;
    return true;
}
static void write_channel(uint8_t index, bool on) {
    bool level = profile.active_high ? on : !on;
    port.write(profile.pins[index], level);
    if (on) mask |= (uint8_t)(1u << index);
    else mask &= (uint8_t)~(1u << index);
}
static uint8_t channel_count(void *context) { (void)context; return running ? profile.channel_count : 0; }
static bool set(void *context, uint8_t channel, bool on) {
    (void)context;
    if (!running || channel >= profile.channel_count) return false;
    write_channel(channel, on);
    return true;
}
static bool set_duty(void *context, uint8_t channel, uint8_t percent) {
    (void)context;
    if (!running || channel >= profile.channel_count || percent > 100) return false;
    duty[channel] = percent;
    write_channel(channel, percent > 0);
    return true;
}
static bool blink(void *context, uint8_t channel, uint16_t on_us, uint16_t off_us, uint8_t count) {
    (void)context;
    if (!running || channel >= profile.channel_count || !count || !port.delay_us) return false;
    for (uint8_t n = 0; n < count; n++) {
        write_channel(channel, true);
        port.delay_us(on_us);
        write_channel(channel, false);
        port.delay_us(off_us);
    }
    return true;
}
static uint8_t get_mask(void *context) { (void)context; return running ? mask : 0; }
static const led_api_v1 api = {
    LED_API_V1, sizeof(led_api_v1), NULL,
    channel_count, set, set_duty, blink, get_mask
};
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    (void)deps;
    if (running || count || !have_profile) return false;
#if defined(GARDEN_RTE_TARGET_ESP32S3)
    if (!have_port) { port = esp_port; have_port = true; }
#endif
    if (!have_port) return false;
    for (uint8_t i = 0; i < profile.channel_count; i++) {
        if (!port.claim_output(profile.pins[i])) {
            for (uint8_t j = 0; j < i; j++) { write_channel(j, false); port.release(profile.pins[j]); }
            return false;
        }
        duty[i] = 0;
        write_channel(i, false);
    }
    mask = 0;
    running = true;
    return true;
}
static bool quiesce(void) {
    if (!running) return true;
    if (port.write) for (uint8_t i = 0; i < profile.channel_count; i++) write_channel(i, false);
    mask = 0;
    return true;
}
static void stop(void) {
    (void)quiesce();
    if (port.release) for (uint8_t i = 0; i < profile.channel_count; i++) port.release(profile.pins[i]);
    running = false;
}
static const risc_driver_v2 driver = {
    RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2),
    "led", "indicator.led", LED_API_V1,
    &api, start, stop, quiesce
};
__attribute__((visibility("default")))
const risc_driver_v2 *t5_driver_get(uint32_t abi) {
    return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL;
}
