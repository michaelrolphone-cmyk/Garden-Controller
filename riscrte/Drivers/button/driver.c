/* Generic RiscRTE button driver. Publishes input.button@1.
 * Does not know what a press means. Pins come from a bound profile. */
#include "ButtonApi.h"
#include "../../sdk/RiscProviderV2.h"

typedef struct {
    bool (*claim_input)(uint8_t pin);
    bool (*read)(uint8_t pin);
    void (*release)(uint8_t pin);
} button_gpio_port_t;
typedef struct {
    bool down;
    bool stable;
    uint32_t change_us;
    uint32_t down_us;
    bool click_pending;
    bool long_pending;
    bool long_sent;
} button_channel_t;
static bool running;
static button_profile_t profile;
static button_gpio_port_t port;
static button_channel_t channels[BUTTON_CHANNELS_MAX];
static bool have_profile;
static bool have_port;
#if defined(GARDEN_RTE_TARGET_ESP32S3)
#include "driver/gpio.h"
static bool esp_claim(uint8_t pin) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg) == ESP_OK;
}
static bool esp_read(uint8_t pin) { return gpio_get_level((gpio_num_t)pin) != 0; }
static void esp_release(uint8_t pin) { gpio_reset_pin((gpio_num_t)pin); }
static const button_gpio_port_t esp_port = {esp_claim, esp_read, esp_release};
#endif
bool button_bind_port(const button_gpio_port_t *next) {
    if (running || !next || !next->claim_input || !next->read || !next->release) return false;
    port = *next; have_port = true; return true;
}
bool button_bind_profile(const button_profile_t *next) {
    if (running || !next || next->channel_count == 0 || next->channel_count > BUTTON_CHANNELS_MAX) return false;
    for (uint8_t i = 0; i < next->channel_count; i++) {
        for (uint8_t j = 0; j < i; j++) if (next->pins[i] == next->pins[j]) return false;
    }
    profile = *next;
    if (!profile.debounce_us) profile.debounce_us = 30000u;
    if (!profile.long_press_us) profile.long_press_us = 1500000u;
    if (!profile.click_min_us) profile.click_min_us = 30000u;
    have_profile = true;
    return true;
}
static bool raw_down(uint8_t index) {
    bool level = port.read(profile.pins[index]);
    return profile.active_low ? !level : level;
}
static uint8_t channel_count(void *context) { (void)context; return running ? profile.channel_count : 0; }
static void poll(void *context, uint32_t now_us) {
    (void)context;
    if (!running) return;
    for (uint8_t i = 0; i < profile.channel_count; i++) {
        bool down = raw_down(i);
        button_channel_t *ch = &channels[i];
        if (down != ch->stable) {
            if ((uint32_t)(now_us - ch->change_us) >= profile.debounce_us) {
                ch->stable = down;
                ch->change_us = now_us;
                if (down) { ch->down = true; ch->down_us = now_us; ch->long_sent = false; }
                else {
                    if (ch->down && !ch->long_sent && (uint32_t)(now_us - ch->down_us) >= profile.click_min_us) ch->click_pending = true;
                    ch->down = false;
                }
            }
        } else ch->change_us = now_us;
        if (ch->down && !ch->long_sent && (uint32_t)(now_us - ch->down_us) >= profile.long_press_us) {
            ch->long_pending = true;
            ch->long_sent = true;
        }
    }
}
static bool is_down(void *context, uint8_t channel) {
    (void)context;
    return running && channel < profile.channel_count && channels[channel].down;
}
static bool take_click(void *context, uint8_t channel) {
    (void)context;
    if (!running || channel >= profile.channel_count) return false;
    bool v = channels[channel].click_pending;
    channels[channel].click_pending = false;
    return v;
}
static bool take_long_press(void *context, uint8_t channel) {
    (void)context;
    if (!running || channel >= profile.channel_count) return false;
    bool v = channels[channel].long_pending;
    channels[channel].long_pending = false;
    return v;
}
static const button_api_v1 api = {
    BUTTON_API_V1, sizeof(button_api_v1), NULL,
    channel_count, poll, is_down, take_click, take_long_press
};
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    (void)deps;
    if (running || count || !have_profile) return false;
#if defined(GARDEN_RTE_TARGET_ESP32S3)
    if (!have_port) { port = esp_port; have_port = true; }
#endif
    if (!have_port) return false;
    for (uint8_t i = 0; i < profile.channel_count; i++) {
        if (!port.claim_input(profile.pins[i])) {
            for (uint8_t j = 0; j < i; j++) port.release(profile.pins[j]);
            return false;
        }
        channels[i].stable = raw_down(i);
        channels[i].down = channels[i].stable;
        channels[i].change_us = 0;
        channels[i].click_pending = channels[i].long_pending = channels[i].long_sent = false;
    }
    running = true;
    return true;
}
static bool quiesce(void) {
    for (uint8_t i = 0; i < profile.channel_count; i++) {
        channels[i].click_pending = false;
        channels[i].long_pending = false;
    }
    return true;
}
static void stop(void) {
    (void)quiesce();
    if (port.release) for (uint8_t i = 0; i < profile.channel_count; i++) port.release(profile.pins[i]);
    running = false;
}
static const risc_driver_v2 driver = {
    RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2),
    "button", "input.button", BUTTON_API_V1,
    &api, start, stop, quiesce
};
__attribute__((visibility("default")))
const risc_driver_v2 *t5_driver_get(uint32_t abi) {
    return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL;
}
