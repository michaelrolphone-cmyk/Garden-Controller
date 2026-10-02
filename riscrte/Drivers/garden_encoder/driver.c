/* garden-encoder RiscRTE driver ELF for the CrowPanel rotary knob.
 * Owns quadrature decode and button timing. The app only sees detents and
 * click/long-press edges. */
#include "GardenEncoderApi.h"
#include "../../sdk/RiscProviderV2.h"

#define ENCODER_A_PIN 45
#define ENCODER_B_PIN 42
#define ENCODER_BTN_PIN 41
#define ENCODER_EDGES_PER_DETENT 2
#define ENCODER_DEBOUNCE_US 800u
#define ENCODER_LONG_PRESS_US 1500000u

typedef struct {
    bool (*claim_input)(uint8_t pin);
    bool (*read)(uint8_t pin);
    void (*release)(uint8_t pin);
} garden_gpio_in_port_t;

static bool running;
static garden_gpio_in_port_t port;
static int edge_accum;
static int detent_accum;
static uint32_t last_edge_us;
static bool last_a;
static bool button_down;
static uint32_t button_down_us;
static bool click_pending;
static bool long_pending;
static bool long_sent;

bool garden_encoder_bind_port(const garden_gpio_in_port_t *next) {
    if (running || !next || !next->claim_input || !next->read || !next->release) return false;
    port = *next;
    return true;
}
#if defined(GARDEN_RTE_TARGET_ESP32S3)
#include "driver/gpio.h"
static bool esp_claim_in(uint8_t pin) {
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
static const garden_gpio_in_port_t esp_port = {esp_claim_in, esp_read, esp_release};
#endif
static int32_t take_detents(void *context) { (void)context; int32_t n = detent_accum; detent_accum = 0; return n; }
static bool button_pressed(void *context) { (void)context; return button_down; }
static bool take_click(void *context) { (void)context; bool v = click_pending; click_pending = false; return v; }
static bool take_long_press(void *context) { (void)context; bool v = long_pending; long_pending = false; return v; }
static void poll(void *context, uint32_t now_us) {
    (void)context;
    if (!running) return;
    bool a = port.read(ENCODER_A_PIN);
    bool b = port.read(ENCODER_B_PIN);
    if (a != last_a) {
        if ((uint32_t)(now_us - last_edge_us) >= ENCODER_DEBOUNCE_US) {
            last_edge_us = now_us;
            edge_accum += (a == b) ? 1 : -1;
            if (edge_accum >= ENCODER_EDGES_PER_DETENT) { detent_accum++; edge_accum = 0; }
            else if (edge_accum <= -ENCODER_EDGES_PER_DETENT) { detent_accum--; edge_accum = 0; }
        }
        last_a = a;
    }
    bool pressed = !port.read(ENCODER_BTN_PIN);
    if (pressed && !button_down) { button_down = true; button_down_us = now_us; long_sent = false; }
    else if (pressed && button_down && !long_sent && (uint32_t)(now_us - button_down_us) >= ENCODER_LONG_PRESS_US) {
        long_pending = true; long_sent = true;
    } else if (!pressed && button_down) {
        if (!long_sent && (uint32_t)(now_us - button_down_us) >= 30000u) click_pending = true;
        button_down = false;
    }
}
static const garden_encoder_api_v1 api = {
    GARDEN_ENCODER_API_V1, sizeof(garden_encoder_api_v1), NULL,
    take_detents, button_pressed, take_click, take_long_press, poll
};
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    (void)deps;
    if (running || count) return false;
#if defined(GARDEN_RTE_TARGET_ESP32S3)
    if (!port.claim_input) port = esp_port;
#endif
    if (!port.claim_input || !port.read || !port.release) return false;
    if (!port.claim_input(ENCODER_A_PIN) || !port.claim_input(ENCODER_B_PIN) || !port.claim_input(ENCODER_BTN_PIN)) {
        port.release(ENCODER_A_PIN); port.release(ENCODER_B_PIN); port.release(ENCODER_BTN_PIN);
        return false;
    }
    last_a = port.read(ENCODER_A_PIN);
    edge_accum = detent_accum = 0;
    last_edge_us = 0;
    button_down = click_pending = long_pending = long_sent = false;
    running = true;
    return true;
}
static bool quiesce(void) {
    edge_accum = detent_accum = 0;
    click_pending = long_pending = false;
    return true;
}
static void stop(void) {
    (void)quiesce();
    if (port.release) { port.release(ENCODER_A_PIN); port.release(ENCODER_B_PIN); port.release(ENCODER_BTN_PIN); }
    running = false;
    button_down = false;
}
static const risc_driver_v2 driver = {
    RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2),
    "garden-encoder", "input.quadrature", GARDEN_ENCODER_API_V1,
    &api, start, stop, quiesce
};
__attribute__((visibility("default")))
const risc_driver_v2 *t5_driver_get(uint32_t abi) {
    return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL;
}
