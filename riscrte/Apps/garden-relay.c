/* Same garden relay app as before. The only change is the runtime entry:
 * risc_runtime_get_api instead of t5_app_get_api / t5_provider_capability_get_api.
 * Zone policy, relay mask, and buzzer chirp are unchanged. */
#include "RiscRuntimeV1.h"
#include "garden_policy.h"
#include "../Drivers/buzzer/BuzzerApi.h"
#include "../Drivers/relay/RelayApi.h"

static const risc_runtime_api_v1 *runtime;
static const relay_api_v1 *relay;
static const buzzer_api_v1 *buzzer;
static risc_runtime_capability_v1 relay_grant;
static risc_runtime_capability_v1 buzzer_grant;
static bool has_relay;
static bool has_buzzer;
static garden_policy_t policy;

static void release_all(void) {
    if (relay && relay->set_mask) relay->set_mask(relay->context, 0);
    if (buzzer && buzzer->set) buzzer->set(buzzer->context, 0, false);
    if (has_buzzer) runtime->release(&buzzer_grant);
    if (has_relay) runtime->release(&relay_grant);
    has_relay = false;
    has_buzzer = false;
}

__attribute__((visibility("default"))) void app_main(void) {
    runtime = risc_runtime_get_api(1);
    if (!runtime || runtime->api_version != 1 ||
        runtime->struct_size < RISC_RUNTIME_CAPABILITIES_V1_SIZE ||
        !runtime->health || !runtime->yield_ms || !runtime->diagnostic ||
        !runtime->acquire || !runtime->release) {
        return;
    }

    relay_grant.struct_size = sizeof(relay_grant);
    buzzer_grant.struct_size = sizeof(buzzer_grant);
    has_relay = runtime->acquire("switch.relay", RELAY_API_V1, 0, &relay_grant);
    relay = has_relay ? relay_grant.api : NULL;
    if (!has_relay || !relay || !relay->set_mask) {
        runtime->diagnostic("GARDEN_RELAY missing switch.relay");
        release_all();
        return;
    }
    has_buzzer = runtime->acquire("sound.buzzer", BUZZER_API_V1, 0, &buzzer_grant);
    buzzer = has_buzzer ? buzzer_grant.api : NULL;
    if (!has_buzzer || !buzzer) {
        runtime->diagnostic("GARDEN_RELAY missing sound.buzzer");
        release_all();
        return;
    }

    garden_policy_init(&policy);
    garden_policy_start_zone(&policy, 1, GARDEN_DEFAULT_RUN_SEC);
    relay->set_mask(relay->context, garden_policy_relay_mask(&policy));
    if (buzzer->chirp) buzzer->chirp(buzzer->context, 0);
    runtime->diagnostic("GARDEN_RELAY running");

    risc_runtime_health_v1 health = {.struct_size = sizeof(health)};
    uint32_t last = 0;
    if (runtime->health(&health)) last = health.uptime_ms;
    for (;;) {
        runtime->yield_ms(50);
        if (!runtime->health(&health)) continue;
        if ((uint32_t)(health.uptime_ms - last) < 1000u) continue;
        garden_policy_tick(&policy);
        relay->set_mask(relay->context, garden_policy_relay_mask(&policy));
        last = health.uptime_ms;
    }
}
