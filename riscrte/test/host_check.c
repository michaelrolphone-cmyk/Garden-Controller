#include "../include/garden_policy.h"
#include "../Drivers/garden_encoder/GardenEncoderApi.h"
#include "../Drivers/garden_relay6/GardenRelayApi.h"
#include "../sdk/RiscProviderV2.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void expect(int cond, const char *msg) {
    if (!cond) { fprintf(stderr, "FAIL %s\n", msg); failures++; }
}
static bool levels[64];
static bool claimed[64];
static bool claim_out(uint8_t pin) { claimed[pin] = true; return true; }
static void write_out(uint8_t pin, bool level) { levels[pin] = level; }
static void release_out(uint8_t pin) { claimed[pin] = false; levels[pin] = false; }
static void delay_us(uint32_t us) { (void)us; }
static bool in_level[64];
static bool claim_in(uint8_t pin) { claimed[pin] = true; return true; }
static bool read_in(uint8_t pin) { return in_level[pin]; }
static void release_in(uint8_t pin) { claimed[pin] = false; }
extern const risc_driver_v2 *relay_get(uint32_t abi);
extern const risc_driver_v2 *encoder_get(uint32_t abi);
extern bool garden_relay_bind_port(const void *port);
extern bool garden_encoder_bind_port(const void *port);
typedef struct { bool (*claim_output)(uint8_t); void (*write)(uint8_t, bool); void (*release)(uint8_t); void (*delay_us)(uint32_t); } out_port_t;
typedef struct { bool (*claim_input)(uint8_t); bool (*read)(uint8_t); void (*release)(uint8_t); } in_port_t;
static void test_policy(void) {
    garden_policy_t p; garden_policy_init(&p);
    garden_schedule_t a = {1, 6, 30, 60, true};
    garden_schedule_t b = {3, 6, 30, 120, true};
    garden_schedule_t bad = {6, 6, 30, 60, true};
    expect(garden_policy_add_schedule(&p, a), "add zone 1");
    expect(garden_policy_add_schedule(&p, b), "add zone 3");
    expect(!garden_policy_add_schedule(&p, bad), "reject master as schedule");
    expect(garden_policy_apply_due(&p, 6, 30) == 2, "both due entries start");
    expect(garden_policy_apply_due(&p, 6, 30) == 0, "same minute fires once");
    expect(garden_policy_relay_mask(&p) == 0x25, "zones 1 and 3 plus master");
    garden_policy_start_zone(&p, 2, 2);
    expect(garden_policy_relay_mask(&p) == 0x27, "overlapping zone 2");
    garden_policy_tick(&p); garden_policy_tick(&p);
    expect(!p.zone_on[1], "zone 2 timed out");
    expect(p.zone_on[0] && p.zone_on[2], "other zones stay on");
    garden_policy_all_off(&p);
    expect(garden_policy_relay_mask(&p) == 0, "all off");
}
static void test_relay_safe_off(void) {
    out_port_t port = {claim_out, write_out, release_out, delay_us};
    memset(levels, 0, sizeof(levels)); memset(claimed, 0, sizeof(claimed));
    expect(garden_relay_bind_port(&port), "bind relay port");
    const risc_driver_v2 *d = relay_get(2);
    expect(d && d->start(NULL, 0), "relay start");
    const garden_relay_api_v1 *api = d->capability;
    expect(api->set_mask(NULL, 0x21), "set zone1+master");
    expect(levels[1] && levels[46] && !levels[2], "gpio levels");
    expect(d->quiesce(), "quiesce");
    expect(!levels[1] && !levels[46], "quiesce drops valves");
    d->stop();
    expect(!claimed[1] && !claimed[21], "pins released");
}
static void test_encoder(void) {
    in_port_t port = {claim_in, read_in, release_in};
    memset(in_level, 1, sizeof(in_level));
    expect(garden_encoder_bind_port(&port), "bind encoder port");
    const risc_driver_v2 *d = encoder_get(2);
    expect(d && d->start(NULL, 0), "encoder start");
    const garden_encoder_api_v1 *api = d->capability;
    in_level[45] = false; in_level[42] = false; api->poll(NULL, 1000);
    in_level[45] = true; in_level[42] = true; api->poll(NULL, 2000);
    expect(api->take_detents(NULL) == 1, "two edges one detent");
    in_level[41] = false; api->poll(NULL, 3000); api->poll(NULL, 3000 + 1500000);
    expect(api->take_long_press(NULL), "long press");
    in_level[41] = true; api->poll(NULL, 5000000);
    expect(!api->take_click(NULL), "long press does not also click");
    d->stop();
}
int main(void) {
    test_policy(); test_relay_safe_off(); test_encoder();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("garden riscrte host checks passed");
    return 0;
}
