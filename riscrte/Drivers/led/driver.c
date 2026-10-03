/* Generic RiscRTE LED indicator driver. Publishes indicator.led@1.
 * Does not know what a channel means. Pins come from a bound profile.
 * Quiesce and stop force every channel off. */
#include "LedApi.h"
#include "../../sdk/RiscProviderV2.h"
#include "../common/gpio.h"

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
    return !io_fault;
}
static bool set_duty(void *context, uint8_t channel, uint8_t percent) {
    (void)context;
    if (!running || channel >= profile.channel_count || percent > 100) return false;
    if (!gpio->pwm(gpio->context,pins[profile.pins[channel]],1000,profile.active_high?percent:100-percent,100)) return false;
    duty[channel] = percent;
    if (percent) mask |= (uint8_t)(1u<<channel); else mask &= (uint8_t)~(1u<<channel);
    return true;
}
static bool blink(void *context,uint8_t channel,uint16_t on_us,uint16_t off_us,uint8_t count) {
    (void)context;
    if (!running || channel>=profile.channel_count || !count || (uint64_t)(on_us+off_us)*count>20000) return false;
    uint32_t pulses[512];
    size_t offset=profile.active_high?0:1; pulses[0]=0;
    for (size_t i=0;i<count;i++) { pulses[offset+i*2]=(uint32_t)on_us*1000; pulses[offset+i*2+1]=(uint32_t)off_us*1000; }
    size_t length=(size_t)count*2+offset;
    bool ok=gpio->waveform(gpio->context,pins[profile.pins[channel]],pulses,length);
    write_channel(channel,false); return ok && !io_fault;
}
static uint8_t get_mask(void *context) { (void)context; return running ? mask : 0; }
static const led_api_v1 api = {
    LED_API_V1, sizeof(led_api_v1), NULL,
    channel_count, set, set_duty, blink, get_mask
};
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    if (running || !gpio_clean()) return false;
    const risc_hw_gpio_bank_v1 *config=hardware_config(deps,count,"generic,gpio-led-bank","gpio.bank",sizeof(*config));
    if (!hw_bank(config,8)) return false;
    profile.channel_count=config->count;
    for(size_t i=0;i<config->count;i++) profile.pins[i]=(uint8_t)config->pins[i];
    profile.active_high=config->active_high;
    gpio_output_initial=!config->active_high; gpio_input_pullup=config->pull_up; have_profile=true;
    if (!gpio_dependencies(deps,count)) return false;
    port = (led_gpio_port_t){gpio_output, gpio_write, gpio_release, gpio_delay}; have_port = true;

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
    running = !io_fault;
    return running;
}
static bool quiesce(void) {
    running=false; io_fault=false;
    for (uint8_t i=0;i<profile.channel_count;i++) if (pins[profile.pins[i]]) gpio_write(profile.pins[i],!profile.active_high);
    if (io_fault) return false;
    for (uint8_t pin=0;pin<49;pin++) gpio_release(pin);
    return gpio_clean();
}
static void stop(void) { (void)quiesce(); }
static const risc_driver_v2 driver = {
    RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2),
    "led", "indicator.led", LED_API_V1,
    &api, start, stop, quiesce
};
__attribute__((visibility("default")))
const risc_driver_v2 *t5_driver_get(uint32_t abi) {
    return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL;
}
