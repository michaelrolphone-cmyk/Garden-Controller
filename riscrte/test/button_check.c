#include "../Drivers/button/ButtonApi.h"
#include "../sdk/RiscProviderV2.h"
#include <stdio.h>
#include <string.h>
static int failures;
static void expect(int cond, const char *msg) { if (!cond) { fprintf(stderr, "FAIL %s\n", msg); failures++; } }
static bool levels[64];
static bool claimed[64];
static bool claim_in(uint8_t pin) { claimed[pin] = true; return true; }
static bool read_in(uint8_t pin) { return levels[pin]; }
static void release_in(uint8_t pin) { claimed[pin] = false; }
extern const risc_driver_v2 *button_get(uint32_t abi);
extern bool button_bind_port(const void *port);
extern bool button_bind_profile(const button_profile_t *profile);
typedef struct { bool (*claim_input)(uint8_t); bool (*read)(uint8_t); void (*release)(uint8_t); } in_port_t;
int main(void) {
    in_port_t port = {claim_in, read_in, release_in};
    button_profile_t board = { .channel_count = 1, .pins = {41}, .active_low = true, .debounce_us = 30000, .long_press_us = 1500000, .click_min_us = 30000 };
    levels[41] = true;
    expect(button_bind_port(&port), "bind port");
    expect(button_bind_profile(&board), "bind profile");
    const risc_driver_v2 *d = button_get(2);
    expect(d && strcmp(d->capability_id, "input.button") == 0, "capability");
    expect(d->start(NULL, 0), "start");
    const button_api_v1 *api = d->capability;
    levels[41] = false;
    api->poll(NULL, 1000);
    api->poll(NULL, 40000);
    expect(api->is_down(NULL, 0), "down after debounce");
    api->poll(NULL, 1600000);
    expect(api->take_long_press(NULL, 0), "long press");
    levels[41] = true;
    api->poll(NULL, 1700000);
    expect(!api->take_click(NULL, 0), "long press is not a click");
    d->stop();
    expect(!claimed[41], "released");
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("button host check passed");
    return 0;
}
