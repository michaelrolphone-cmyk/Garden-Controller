#pragma once
#include "hardware.h"
#include "RiscPlatformClockV1.h"
static const garden_gpio_v1 *gpio;
static const risc_platform_clock_api_v1 *timer;
static uint64_t pins[49];
static bool io_fault;
static bool gpio_output_initial, gpio_input_pullup=true;
static inline bool gpio_dependencies(const risc_provider_dependency_v1 *d, size_t n) {
    for (size_t i=0;i<49;i++) if (pins[i]) return false;
    gpio=garden_dependency(d,n,"platform.gpio",sizeof(*gpio));
    timer=garden_dependency(d,n,"platform.clock",sizeof(*timer));
    io_fault=false;
    return gpio && gpio->claim && gpio->write && gpio->read && gpio->pwm && gpio->release && gpio->waveform && timer && timer->monotonic_ms && timer->sleep_ms;
}
static inline bool gpio_claim(uint8_t pin, bool output, bool initial) {
    if (!gpio || pin>=49 || pins[pin]) return false;
    return gpio->claim(gpio->context,pin,output,initial,!output && gpio_input_pullup,&pins[pin]) && pins[pin];
}
static inline bool gpio_output(uint8_t pin) { return gpio_claim(pin,true,gpio_output_initial); }
static inline bool gpio_input(uint8_t pin) { return gpio_claim(pin,false,false); }
static inline void gpio_write(uint8_t pin,bool level) {
    if (pin>=49 || !pins[pin] || !gpio->write(gpio->context,pins[pin],level)) io_fault=true;
}
static inline bool gpio_read(uint8_t pin) {
    bool level=true;
    if (pin>=49 || !pins[pin] || !gpio->read(gpio->context,pins[pin],&level)) io_fault=true;
    return level;
}
static inline void gpio_release(uint8_t pin) {
    if (pin<49 && pins[pin]) {
        if (gpio->release(gpio->context,pins[pin])) pins[pin]=0;
        else io_fault=true;
    }
}
static inline void gpio_delay(uint32_t us) {
    /* Cooperative delay, minimum 1ms. Precise pulses use waveform(). */
    timer->sleep_ms(timer->context,(us+999)/1000);
}
static inline bool gpio_clean(void) {
    for (size_t i=0;i<49;i++) if (pins[i]) return false;
    return true;
}
