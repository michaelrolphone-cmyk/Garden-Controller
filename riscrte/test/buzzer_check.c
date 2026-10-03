#include "../Drivers/buzzer/BuzzerApi.h"
#include "../sdk/RiscProviderV2.h"
#include <stdio.h>
#include <string.h>
static int failures;
static void expect(int cond, const char *msg) { if (!cond) { fprintf(stderr, "FAIL %s\n", msg); failures++; } }
static bool levels[64];
static bool claimed[64];
static uint32_t delayed;
static uint32_t on_us_seen;
static bool claim_out(uint8_t pin) { claimed[pin] = true; return true; }
static void write_out(uint8_t pin, bool level) { levels[pin] = level; if (level) on_us_seen++; }
static void release_out(uint8_t pin) { claimed[pin] = false; levels[pin] = false; }
static void delay_us(uint32_t us) { delayed += us; }
extern const risc_driver_v2 *buzzer_get(uint32_t abi);
extern bool buzzer_bind_port(const void *port);
extern bool buzzer_bind_profile(const buzzer_profile_t *profile);
typedef struct { bool (*claim_output)(uint8_t); void (*write)(uint8_t, bool); void (*release)(uint8_t); void (*delay_us)(uint32_t); } out_port_t;
int main(void) {
    out_port_t port = {claim_out, write_out, release_out, delay_us};
    buzzer_profile_t board = { .channel_count = 1, .pins = {21}, .active_high = true };
    expect(buzzer_bind_port(&port), "bind port");
    expect(buzzer_bind_profile(&board), "bind profile");
    const risc_driver_v2 *d = buzzer_get(2);
    expect(d && strcmp(d->capability_id, "sound.buzzer") == 0, "capability");
    expect(d->start(NULL, 0), "start");
    const buzzer_api_v1 *api = d->capability;
    expect(api->chirp(NULL, 0), "chirp");
    expect(!levels[21], "ends off");
    expect(on_us_seen == 8, "eight on pulses");
    expect(delayed == 8000, "8 ms at 5 percent");
    expect(d->quiesce(), "quiesce");
    d->stop();
    expect(!claimed[21], "released");
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("buzzer host check passed");
    return 0;
}
