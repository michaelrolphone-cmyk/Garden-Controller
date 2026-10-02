#include <T5AppApi.h>
#include <T5ProviderCapabilityApi.h>
#include "garden_policy.h"
#include "../Drivers/buzzer/BuzzerApi.h"
#include "../Drivers/relay/RelayApi.h"
#include <stdio.h>
#include <string.h>

static const t5_app_api_v1 *app;
static const t5_provider_capability_api_v1 *providers;
static const relay_api_v1 *relay;
static const buzzer_api_v1 *buzzer;
static t5_provider_capability_lease_t relay_lease;
static t5_provider_capability_lease_t buzzer_lease;
static garden_policy_t policy;

static bool lease_one(const char *capability, uint32_t version, t5_provider_capability_lease_t *lease, const void **out) {
    *lease = T5_PROVIDER_CAPABILITY_LEASE_INVALID;
    *out = NULL;
    if (!providers->acquire(capability, version, lease, out) || !*out) {
        printf("GARDEN_RELAY missing %s\n", capability);
        return false;
    }
    return true;
}

static void release_all(void) {
    if (relay && relay->set_mask) relay->set_mask(relay->context, 0);
    if (buzzer && buzzer->set) buzzer->set(buzzer->context, 0, false);
    if (relay_lease) providers->release(relay_lease);
    if (buzzer_lease) providers->release(buzzer_lease);
}

void app_main(void) {
    app = t5_app_get_api(T5_APP_ABI_VERSION);
    providers = t5_provider_capability_get_api(T5_PROVIDER_CAPABILITY_API_VERSION);
    if (!app || !app->poll || !app->millis || !providers || !providers->acquire || !providers->release) {
        printf("GARDEN_RELAY unavailable-api\n");
        return;
    }
    const void *borrowed = NULL;
    if (!lease_one("switch.relay", RELAY_API_V1, &relay_lease, &borrowed)) return;
    relay = borrowed;
    if (!lease_one("sound.buzzer", BUZZER_API_V1, &buzzer_lease, &borrowed)) { release_all(); return; }
    buzzer = borrowed;
    if (!relay->set_mask) { release_all(); return; }
    garden_policy_init(&policy);
    garden_policy_start_zone(&policy, 1, GARDEN_DEFAULT_RUN_SEC);
    relay->set_mask(relay->context, garden_policy_relay_mask(&policy));
    if (buzzer->chirp) buzzer->chirp(buzzer->context, 0);
    printf("GARDEN_RELAY running\n");
    uint32_t last = app->millis();
    for (;;) {
        t5_app_input_t input;
        memset(&input, 0, sizeof(input));
        if (!app->poll(&input, 50) || input.exit_requested || (input.buttons & T5_APP_BUTTON_BACK)) break;
        uint32_t now = app->millis();
        if ((uint32_t)(now - last) >= 1000u) {
            garden_policy_tick(&policy);
            relay->set_mask(relay->context, garden_policy_relay_mask(&policy));
            last = now;
        }
    }
    garden_policy_all_off(&policy);
    relay->set_mask(relay->context, 0);
    release_all();
    printf("GARDEN_RELAY stopped\n");
}
