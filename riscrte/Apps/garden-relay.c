/* Relay boot app for generic RiscRTE. risc_runtime_get_api comes from
 * riscrte/Apps/RiscRuntimeV1.h, copied byte-for-byte from
 * michaelrolphone-cmyk/RiscRTE sdk/app/RiscRuntimeV1.h at
 * feat/minimal-runtime e27d3d089086d79f06edeff4c2bd35f6e6243444
 * (same blob as RiscRTE-T-Watch-S3 sdk/app/RiscRuntimeV1.h on
 * codex/watch-clock-app 3157fdc6f0e65caa3f81e5f3305927b2b27ca675).
 * Holds switch.relay@1 and sound.buzzer@1 and stays resident.
 * Does not start a zone, and does not energize a relay or the buzzer.
 * Returning from app_main would idle the runtime, so the success path yields. */
#include "RiscRuntimeV1.h"
#include "../Drivers/buzzer/BuzzerApi.h"
#include "../Drivers/relay/RelayApi.h"

static bool relay_valid(const relay_api_v1 *relay) {
    return relay && relay->api_version == RELAY_API_V1 &&
           relay->struct_size >= sizeof(*relay) && relay->context &&
           relay->channel_count && relay->set_channel && relay->set_mask &&
           relay->get_mask;
}

static bool buzzer_valid(const buzzer_api_v1 *buzzer) {
    return buzzer && buzzer->api_version == BUZZER_API_V1 &&
           buzzer->struct_size >= sizeof(*buzzer) && buzzer->context &&
           buzzer->channel_count && buzzer->set && buzzer->is_on;
}

__attribute__((visibility("default"))) void app_main(void) {
    const risc_runtime_api_v1 *runtime = risc_runtime_get_api(1);
    if (!runtime || runtime->api_version != 1 ||
        runtime->struct_size < RISC_RUNTIME_CAPABILITIES_V1_SIZE ||
        !runtime->health || !runtime->yield_ms || !runtime->diagnostic ||
        !runtime->acquire || !runtime->release) return;

    risc_runtime_capability_v1 relay_grant = {.struct_size = sizeof(relay_grant)};
    risc_runtime_capability_v1 buzzer_grant = {.struct_size = sizeof(buzzer_grant)};
    bool has_relay = runtime->acquire("switch.relay", 1, 0, &relay_grant);
    bool has_buzzer = runtime->acquire("sound.buzzer", 1, 0, &buzzer_grant);
    const relay_api_v1 *relay = has_relay ? relay_grant.api : NULL;
    const buzzer_api_v1 *buzzer = has_buzzer ? buzzer_grant.api : NULL;
    risc_runtime_health_v1 health = {.struct_size = sizeof(health)};
    /* Table checks only. set_mask, set, chirp, and pattern would drive pins. */
    if (!has_relay || !relay_valid(relay) || !has_buzzer || !buzzer_valid(buzzer) ||
        !runtime->health(&health)) {
        runtime->diagnostic("GARDEN_RELAY error=grant-or-api");
        if (has_buzzer) runtime->release(&buzzer_grant);
        if (has_relay) runtime->release(&relay_grant);
        return;
    }
    runtime->diagnostic("GARDEN_RELAY ready channels=off");
    /* yield_ms clamps to 50ms. Stay mapped so the default app does not idle. */
    for (;;) runtime->yield_ms(50);
}
