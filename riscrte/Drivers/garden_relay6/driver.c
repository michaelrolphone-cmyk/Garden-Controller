/* garden-relay6 RiscRTE driver ELF for a 6-channel relay bank.
 * Owns the GPIO outputs. Does not interpret what a channel switches.
 * Safe-off is mandatory on quiesce/stop so an unload cannot leave a contact closed. */
#include "GardenRelayApi.h"
#include "../../sdk/RiscProviderV2.h"
#include <string.h>

#define RELAY_ON 1
#define BUZZER_PIN 21
#define BUZZER_DUTY_PERCENT 5
static const uint8_t RELAY_PINS[GARDEN_RELAY_CHANNELS] = {1, 2, 41, 42, 45, 46};

typedef struct {
    bool (*claim_output)(uint8_t pin);
    void (*write)(uint8_t pin, bool level);
    void (*release)(uint8_t pin);
    void (*delay_us)(uint32_t us);
} garden_gpio_out_port_t;

static bool running;
static uint8_t mask;
static garden_gpio_out_port_t port;

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
static const garden_gpio_out_port_t esp_port = {esp_claim, esp_write, esp_release, esp_delay_us};
#endif

bool garden_relay_bind_port(const garden_gpio_out_port_t *next) {
    if (running || !next || !next->claim_output || !next->write || !next->release) return false;
    port = *next;
    return true;
}

static void apply_mask(uint8_t next) {
    for (uint8_t i = 0; i < GARDEN_RELAY_CHANNELS; i++) port.write(RELAY_PINS[i], (next & (1u << i)) != 0);
    mask = next;
}
static bool set_mask(void *context, uint8_t next) {
    (void)context;
    if (!running) return false;
    apply_mask(next);
    return true;
}
static uint8_t get_mask(void *context) {
    (void)context;
    return running ? mask : 0;
}
static bool chirp(void *context) {
    (void)context;
    if (!running || !port.delay_us) return false;
    for (uint8_t n = 0; n < 8; n++) {
        port.write(BUZZER_PIN, true);
        port.delay_us(50);
        port.write(BUZZER_PIN, false);
        port.delay_us(950);
    }
    return true;
}
static const garden_relay_api_v1 api = {
    GARDEN_RELAY_API_V1, sizeof(garden_relay_api_v1), NULL, set_mask, get_mask, chirp
};
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    (void)deps;
    if (running || count) return false;
#if defined(GARDEN_RTE_TARGET_ESP32S3)
    if (!port.claim_output) port = esp_port;
#endif
    if (!port.claim_output || !port.write || !port.release) return false;
    if (!port.claim_output(BUZZER_PIN)) return false;
    port.write(BUZZER_PIN, false);
    for (uint8_t i = 0; i < GARDEN_RELAY_CHANNELS; i++) {
        if (!port.claim_output(RELAY_PINS[i])) {
            port.write(BUZZER_PIN, false);
            port.release(BUZZER_PIN);
            for (uint8_t j = 0; j < i; j++) {
                port.write(RELAY_PINS[j], false);
                port.release(RELAY_PINS[j]);
            }
            return false;
        }
        port.write(RELAY_PINS[i], false);
    }
    mask = 0;
    running = true;
    return true;
}
static bool quiesce(void) {
    if (!running) return true;
    if (port.write) {
        for (uint8_t i = 0; i < GARDEN_RELAY_CHANNELS; i++) port.write(RELAY_PINS[i], false);
        port.write(BUZZER_PIN, false);
    }
    mask = 0;
    return true;
}
static void stop(void) {
    (void)quiesce();
    if (port.release) {
        for (uint8_t i = 0; i < GARDEN_RELAY_CHANNELS; i++) port.release(RELAY_PINS[i]);
        port.release(BUZZER_PIN);
    }
    running = false;
}
static const risc_driver_v2 driver = {
    RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2),
    "garden-relay6", "switch.relay", GARDEN_RELAY_API_V1,
    &api, start, stop, quiesce
};
__attribute__((visibility("default")))
const risc_driver_v2 *t5_driver_get(uint32_t abi) {
    return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL;
}
