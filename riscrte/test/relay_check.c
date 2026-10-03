#include "../Drivers/relay/RelayApi.h"
#include "../Drivers/garden_relay6/board_profile.h"
#include "../sdk/RiscProviderV2.h"
#include <stdio.h>
#include <string.h>
static int failures;
static void expect(int cond, const char *msg) { if (!cond) { fprintf(stderr, "FAIL %s\n", msg); failures++; } }
static bool levels[64];
static bool claimed[64];
static bool claim_out(uint8_t pin) { claimed[pin] = true; return true; }
static void write_out(uint8_t pin, bool level) { levels[pin] = level; }
static void release_out(uint8_t pin) { claimed[pin] = false; levels[pin] = false; }
static void delay_us(uint32_t us) { (void)us; }
extern const risc_driver_v2 *relay_get(uint32_t abi);
extern bool relay_bind_port(const void *port);
extern bool relay_bind_profile(const relay_profile_t *profile);
typedef struct { bool (*claim_output)(uint8_t); void (*write)(uint8_t, bool); void (*release)(uint8_t); void (*delay_us)(uint32_t); } out_port_t;
int main(void) {
    out_port_t port = {claim_out, write_out, release_out, delay_us};
    relay_profile_t board = relay_profile_castle_hills();
    expect(relay_bind_port(&port), "bind port");
    expect(relay_bind_profile(&board), "bind castle hills profile");
    const risc_driver_v2 *d = relay_get(2);
    expect(d && strcmp(d->capability_id, "switch.relay") == 0, "generic capability");
    expect(d->start(NULL, 0), "start");
    const relay_api_v1 *api = d->capability;
    expect(api->channel_count(NULL) == 6, "six channels");
    expect(api->set_channel(NULL, 0, true), "close channel 1");
    expect(api->set_channel(NULL, 2, true), "close channel 3");
    expect(levels[1] && levels[41] && !levels[2], "profile pins");
    expect(api->get_mask(NULL) == 0x05, "mask");
    expect(d->quiesce(), "quiesce");
    expect(!levels[1] && !levels[41], "contacts open");
    d->stop();
    expect(!claimed[1] && !claimed[21], "released");
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("generic relay host check passed");
    return 0;
}
