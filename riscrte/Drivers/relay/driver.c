/* switch.relay@1. Board authorization and exclusive GPIO claims are mandatory. */
#include "RelayApi.h"
#include "../../sdk/RiscProviderV2.h"
#include "../common/gpio.h"
typedef struct { bool (*claim_output)(uint8_t pin); void (*write)(uint8_t pin, bool level); void (*release)(uint8_t pin); void (*delay_us)(uint32_t us); } relay_gpio_port_t;
static bool running, have_profile, have_port; static uint8_t mask; static relay_profile_t profile; static relay_gpio_port_t port;

bool relay_bind_port(const relay_gpio_port_t *next) { if (running || !next || !next->claim_output || !next->write || !next->release) return false; port = *next; have_port = true; return true; }
bool relay_bind_profile(const relay_profile_t *next) { if (running || !next || next->channel_count == 0 || next->channel_count > RELAY_CHANNELS_MAX) return false; for (uint8_t i=0;i<next->channel_count;i++) { if (next->pins[i]>=49) return false; for (uint8_t j=0;j<i;j++) if (next->pins[i]==next->pins[j]) return false; } profile = *next; have_profile = true; return true; }
static void write_channel(uint8_t index, bool closed) { port.write(profile.pins[index], profile.active_high ? closed : !closed); }
static void apply_mask(uint8_t next) { for (uint8_t i = 0; i < profile.channel_count; i++) write_channel(i, (next & (1u << i)) != 0); mask = next; }
static uint8_t channel_count(void *c) { (void)c; return running ? profile.channel_count : 0; }
static bool set_channel(void *c, uint8_t channel, bool closed) { (void)c; if (!running || channel >= profile.channel_count) return false; if (closed) mask |= (uint8_t)(1u << channel); else mask &= (uint8_t)~(1u << channel); write_channel(channel, closed); return !io_fault; }
static bool set_mask(void *c, uint8_t next) { (void)c; if (!running) return false; uint8_t allowed = profile.channel_count == 8 ? 0xffu : (uint8_t)((1u << profile.channel_count) - 1u); apply_mask(next & allowed); return !io_fault; }
static uint8_t get_mask(void *c) { (void)c; return running ? mask : 0; }
/* Legacy optional sound entry. No buzzer is owned by this contact provider. */
static bool chirp(void *c) { (void)c; return false; }
static const relay_api_v1 api = { RELAY_API_V1, sizeof(relay_api_v1), NULL, channel_count, set_channel, set_mask, get_mask, chirp };
static bool start(const risc_provider_dependency_v1 *deps, size_t count) {
    if (running || !gpio_dependencies(deps,count)) return false;
    if (garden_board(deps,count) != GARDEN_BOARD_RELAY) return false;
    port = (relay_gpio_port_t){gpio_output, gpio_write, gpio_release, gpio_delay}; have_port = true;
    { relay_profile_t field = { .channel_count = 6, .pins = {1, 2, 41, 42, 45, 46}, .active_high = true, .indicator_pin = -1 }; if (!relay_bind_profile(&field)) return false; }

    if (!have_port) return false;
    for (uint8_t i = 0; i < profile.channel_count; i++) { if (!port.claim_output(profile.pins[i])) { for (uint8_t j = 0; j < i; j++) { write_channel(j, false); port.release(profile.pins[j]); } return false; } write_channel(i, false); }
    mask = 0; running = !io_fault; return running;
}
static bool quiesce(void) {
    running=false; io_fault=false;
    for (uint8_t i=0;i<profile.channel_count;i++) if (pins[profile.pins[i]]) gpio_write(profile.pins[i],!profile.active_high);
    if (io_fault) return false;
    for (uint8_t pin=0;pin<49;pin++) gpio_release(pin);
    return gpio_clean();
}
static void stop(void) { (void)quiesce(); }
static const risc_driver_v2 driver = { RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2), "relay", "switch.relay", RELAY_API_V1, &api, start, stop, quiesce };
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL; }
