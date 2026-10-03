/* input.button@1. Authorized CrowPanel knob button, GPIO 41. */
#include "ButtonApi.h"
#include "../../sdk/RiscProviderV2.h"
#include "../common/gpio.h"
typedef struct { bool (*claim_input)(uint8_t pin); bool (*read)(uint8_t pin); void (*release)(uint8_t pin); } button_gpio_port_t;
typedef struct { bool down, stable, click_pending, long_pending, long_sent; uint32_t change_us, down_us; } button_channel_t;
static bool running, have_profile, have_port; static button_profile_t profile; static button_gpio_port_t port; static button_channel_t channels[BUTTON_CHANNELS_MAX];



static bool raw_down(uint8_t index) { bool level = port.read(profile.pins[index]); return profile.active_low ? !level : level; }
static uint8_t channel_count(void *c) { (void)c; return running ? profile.channel_count : 0; }
static void poll(void *c, uint32_t now_us) { (void)c; if (!running) return; for (uint8_t i = 0; i < profile.channel_count; i++) { bool down = raw_down(i); button_channel_t *ch = &channels[i]; if (down != ch->stable) { if ((uint32_t)(now_us - ch->change_us) >= profile.debounce_us) { ch->stable = down; ch->change_us = now_us; if (down) { ch->down = true; ch->down_us = now_us; ch->long_sent = false; } else { if (ch->down && !ch->long_sent && (uint32_t)(now_us - ch->down_us) >= profile.click_min_us) ch->click_pending = true; ch->down = false; } } } else ch->change_us = now_us; if (ch->down && !ch->long_sent && (uint32_t)(now_us - ch->down_us) >= profile.long_press_us) { ch->long_pending = true; ch->long_sent = true; } } }
static bool is_down(void *c, uint8_t channel) { (void)c; return running && channel < profile.channel_count && channels[channel].down; }
static bool take_click(void *c, uint8_t channel) { (void)c; if (!running || channel >= profile.channel_count) return false; bool v = channels[channel].click_pending; channels[channel].click_pending = false; return v; }
static bool take_long_press(void *c, uint8_t channel) { (void)c; if (!running || channel >= profile.channel_count) return false; bool v = channels[channel].long_pending; channels[channel].long_pending = false; return v; }
static const button_api_v1 api = { BUTTON_API_V1, sizeof(button_api_v1), NULL, channel_count, poll, is_down, take_click, take_long_press };
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    if (running || !gpio_clean()) return false;
    const risc_hw_gpio_bank_v1 *config=hardware_config(deps,count,"generic,gpio-button-bank","gpio.bank",sizeof(*config));
    if (!hw_bank(config,8)) return false;
    profile.channel_count=config->count;
    for(size_t i=0;i<config->count;i++) profile.pins[i]=(uint8_t)config->pins[i];
    profile.active_low=!config->active_high; profile.debounce_us=config->debounce_us; profile.long_press_us=config->long_press_us; profile.click_min_us=config->click_min_us;
    if (!profile.debounce_us || profile.long_press_us<profile.debounce_us || profile.click_min_us<profile.debounce_us) return false;
    gpio_output_initial=!config->active_high; gpio_input_pullup=config->pull_up; have_profile=true;
    if (!gpio_dependencies(deps,count)) return false;
    port = (button_gpio_port_t){gpio_input, gpio_read, gpio_release}; have_port = true;

    if (!have_port) return false;
    for (uint8_t i = 0; i < profile.channel_count; i++) { if (!port.claim_input(profile.pins[i])) { for (uint8_t j = 0; j < i; j++) port.release(profile.pins[j]); return false; } channels[i].stable = raw_down(i); channels[i].down = channels[i].stable; channels[i].change_us = channels[i].down_us = (uint32_t)(timer->monotonic_ms(timer->context)*1000); channels[i].click_pending = channels[i].long_pending = channels[i].long_sent = false; }
    running = !io_fault; return running;
}
static bool quiesce(void) {
    running=false; io_fault=false;

    if (io_fault) return false;
    for (uint8_t pin=0;pin<49;pin++) gpio_release(pin);
    return gpio_clean();
}
static void stop(void) { (void)quiesce(); }
static const risc_driver_v2 driver = { RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2), "button", "input.button", BUTTON_API_V1, &api, start, stop, quiesce };
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL; }
