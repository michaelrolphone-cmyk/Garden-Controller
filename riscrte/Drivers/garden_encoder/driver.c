/* garden-encoder RiscRTE driver ELF for the CrowPanel rotary knob.
 * Quadrature only. The knob button is input.button, not this driver. */
#include "GardenEncoderApi.h"
#include "../button/ButtonApi.h"
static const button_api_v1 *buttons;
#include "../../sdk/RiscProviderV2.h"
#include "../common/gpio.h"

static risc_hw_quadrature_v1 config;
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



static int32_t take_detents(void *context) { (void)context; int32_t n = running ? detent_accum : 0; detent_accum = 0; return n; }
static bool button_pressed(void *context) { (void)context; return running && buttons->is_down(buttons->context,0); }
static bool take_click(void *context) { (void)context; return running && buttons->take_click(buttons->context,0); }
static bool take_long_press(void *context) { (void)context; return running && buttons->take_long_press(buttons->context,0); }
static void poll(void *context, uint32_t now_us) {
    (void)context;
    if (!running) return;
    buttons->poll(buttons->context,now_us);
    bool a = port.read(config.a);
    bool b = port.read(config.b);
    if (a != last_a) {
        if ((uint32_t)(now_us - last_edge_us) >= config.debounce_us) {
            last_edge_us = now_us;
            edge_accum += ((a == b) ? 1 : -1)*config.direction;
            if (edge_accum >= config.edges_per_detent) { if (detent_accum < INT32_MAX) detent_accum++; edge_accum = 0; }
            else if (edge_accum <= -config.edges_per_detent) { if (detent_accum > INT32_MIN) detent_accum--; edge_accum = 0; }
        }
        last_a = a;
    }
}
static const garden_encoder_api_v1 api = {
    GARDEN_ENCODER_API_V1, sizeof(garden_encoder_api_v1), NULL,
    take_detents, button_pressed, take_click, take_long_press, poll
};
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    if (running || !gpio_clean()) return false;
    const risc_hw_quadrature_v1 *next=hardware_config(deps,count,"generic,quadrature-encoder","input.quadrature",sizeof(*next));
    if (!next || !hw_pin(next->a) || !hw_pin(next->b) || next->a==next->b || next->pull_up>1 || next->reserved ||
        !next->edges_per_detent || next->edges_per_detent>16 || !next->debounce_us || next->debounce_us>1000000 ||
        (next->direction!=1 && next->direction!=-1) || !next->button_instance_id) return false;
    config=*next; gpio_input_pullup=config.pull_up;
    if (!gpio_dependencies(deps,count)) return false;
    buttons=garden_dependency(deps,count,"input.button",sizeof(*buttons));
    if (!buttons || !buttons->is_down || !buttons->take_click || !buttons->take_long_press || !buttons->poll || !buttons->channel_count || !buttons->channel_count(buttons->context)) return false;
    port = (garden_gpio_in_port_t){gpio_input, gpio_read, gpio_release};

    if (!port.claim_input || !port.read || !port.release) return false;
    if (!port.claim_input(config.a)) return false;
    if (!port.claim_input(config.b)) {
        port.release(config.a);
        return false;
    }
    last_a = port.read(config.a);
    edge_accum = detent_accum = 0;
    last_edge_us = 0;
    running = !io_fault;
    return running;
}
static bool quiesce(void) {
    running=false; io_fault=false;

    if (io_fault) return false;
    for (uint8_t pin=0;pin<49;pin++) gpio_release(pin);
    return gpio_clean();
}
static void stop(void) { (void)quiesce(); }
static const risc_driver_v2 driver = {
    RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2),
    "garden-encoder", "input.quadrature", GARDEN_ENCODER_API_V1,
    &api, start, stop, quiesce
};
__attribute__((visibility("default")))
const risc_driver_v2 *t5_driver_get(uint32_t abi) {
    return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL;
}
