#include "../Drivers/led/LedApi.h"
#include "../Drivers/garden_relay6/led_profile.h"
#include "../sdk/RiscProviderV2.h"
#include <stdio.h>
#include <string.h>
static int failures;
static void expect(int cond, const char *msg) { if (!cond) { fprintf(stderr, "FAIL %s\n", msg); failures++; } }
static bool levels[64];
static bool claimed[64];
static uint32_t delayed;
static bool claim_out(uint8_t pin) { claimed[pin] = true; return true; }
static void write_out(uint8_t pin, bool level) { levels[pin] = level; }
static void release_out(uint8_t pin) { claimed[pin] = false; levels[pin] = false; }
static void delay_us(uint32_t us) { delayed += us; }
extern const risc_driver_v2 *led_get(uint32_t abi);
extern bool led_bind_port(const void *port);
extern bool led_bind_profile(const led_profile_t *profile);
typedef struct { bool (*claim_output)(uint8_t); void (*write)(uint8_t, bool); void (*release)(uint8_t); void (*delay_us)(uint32_t); } out_port_t;
int main(void) {
    out_port_t port = {claim_out, write_out, release_out, delay_us};
    led_profile_t board = led_profile_castle_hills();
    expect(led_bind_port(&port), "bind port");
    expect(led_bind_profile(&board), "bind profile");
    const risc_driver_v2 *d = led_get(2);
    expect(d && strcmp(d->capability_id, "indicator.led") == 0, "capability");
    expect(d->start(NULL, 0), "start");
    const led_api_v1 *api = d->capability;
    expect(api->channel_count(NULL) == 1, "one channel");
    expect(api->set(NULL, 0, true), "on");
    expect(levels[21], "gpio 21 on");
    expect(api->set_duty(NULL, 0, 5), "duty");
    expect(api->blink(NULL, 0, 50, 950, 8), "blink");
    expect(!levels[21], "ends off");
    expect(delayed == 8000, "8 ms pattern");
    expect(d->quiesce(), "quiesce");
    d->stop();
    expect(!claimed[21], "released");
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("led host check passed");
    return 0;
}
