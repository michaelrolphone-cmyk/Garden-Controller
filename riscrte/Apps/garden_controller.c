/* Installable RiscRTE app. Entry is app_main, same as Apps/gps.c and
 * Apps/camera_utility.c. The firmware never calls a garden bind function.
 * Capabilities are leased with t5_provider_capability_get_api(). */
#include <T5AppApi.h>
#include <T5ProviderCapabilityApi.h>
#include "../include/garden_policy.h"
#include "../Drivers/button/ButtonApi.h"
#include "../Drivers/buzzer/BuzzerApi.h"
#include "../Drivers/garden_encoder/GardenEncoderApi.h"
#include "../Drivers/relay/RelayApi.h"
#include <stdio.h>
#include <string.h>

static const t5_app_api_v1 *app;
static const t5_provider_capability_api_v1 *providers;
static const relay_api_v1 *relay;
static const buzzer_api_v1 *buzzer;
static const garden_encoder_api_v1 *encoder;
static const button_api_v1 *button;
static t5_provider_capability_lease_t relay_lease;
static t5_provider_capability_lease_t buzzer_lease;
static t5_provider_capability_lease_t encoder_lease;
static t5_provider_capability_lease_t button_lease;
static garden_policy_t policy;
static uint8_t selected_zone = 1;

static bool lease_one(const char *capability, uint32_t version, t5_provider_capability_lease_t *lease, const void **out) {
    *lease = T5_PROVIDER_CAPABILITY_LEASE_INVALID;
    *out = NULL;
    if (!providers->acquire(capability, version, lease, out) || !*out) {
        printf("GARDEN_APP missing %s\n", capability);
        return false;
    }
    return true;
}

static void release_all(void) {
    if (relay && relay->set_mask) relay->set_mask(relay->context, 0);
    if (buzzer && buzzer->set) buzzer->set(buzzer->context, 0, false);
    if (relay_lease) providers->release(relay_lease);
    if (buzzer_lease) providers->release(buzzer_lease);
    if (encoder_lease) providers->release(encoder_lease);
    if (button_lease) providers->release(button_lease);
    relay_lease = buzzer_lease = encoder_lease = button_lease = T5_PROVIDER_CAPABILITY_LEASE_INVALID;
}

static void push_mask(void) {
    if (relay && relay->set_mask) relay->set_mask(relay->context, garden_policy_relay_mask(&policy));
}

void app_main(void) {
    app = t5_app_get_api(T5_APP_ABI_VERSION);
    providers = t5_provider_capability_get_api(T5_PROVIDER_CAPABILITY_API_VERSION);
    if (!app || !app->poll || !app->millis || !providers || !providers->acquire || !providers->release) {
        printf("GARDEN_APP unavailable-api\n");
        return;
    }
    const void *borrowed = NULL;
    if (!lease_one("switch.relay", RELAY_API_V1, &relay_lease, &borrowed)) return;
    relay = borrowed;
    if (!lease_one("sound.buzzer", BUZZER_API_V1, &buzzer_lease, &borrowed)) { release_all(); return; }
    buzzer = borrowed;
    if (!lease_one("input.quadrature", GARDEN_ENCODER_API_V1, &encoder_lease, &borrowed)) { release_all(); return; }
    encoder = borrowed;
    if (!lease_one("input.button", BUTTON_API_V1, &button_lease, &borrowed)) { release_all(); return; }
    button = borrowed;
    if (!relay->set_mask || !encoder->poll || !encoder->take_detents || !button->poll || !button->take_click || !button->take_long_press) {
        printf("GARDEN_APP capability-shape\n");
        release_all();
        return;
    }
    garden_policy_init(&policy);
    push_mask();
    printf("GARDEN_APP running zone=%u\n", selected_zone);
    uint32_t last_sec = app->millis();
    for (;;) {
        t5_app_input_t input;
        memset(&input, 0, sizeof(input));
        if (!app->poll(&input, 20) || input.exit_requested || (input.buttons & T5_APP_BUTTON_BACK)) break;
        uint32_t now_ms = app->millis();
        uint32_t now_us = app->micros ? app->micros() : now_ms * 1000u;
        encoder->poll(encoder->context, now_us);
        button->poll(button->context, now_us);
        int32_t detents = encoder->take_detents(encoder->context);
        while (detents > 0) { selected_zone = (uint8_t)(selected_zone % GARDEN_ZONE_COUNT + 1); detents--; }
        while (detents < 0) { selected_zone = selected_zone == 1 ? GARDEN_ZONE_COUNT : (uint8_t)(selected_zone - 1); detents++; }
        if (button->take_long_press(button->context, 0)) {
            garden_policy_all_off(&policy);
            if (buzzer && buzzer->chirp) buzzer->chirp(buzzer->context, 0);
        } else if (button->take_click(button->context, 0)) {
            if (policy.zone_on[selected_zone - 1]) garden_policy_stop_zone(&policy, selected_zone);
            else {
                garden_policy_start_zone(&policy, selected_zone, GARDEN_DEFAULT_RUN_SEC);
                if (buzzer && buzzer->chirp) buzzer->chirp(buzzer->context, 0);
            }
        }
        if ((uint32_t)(now_ms - last_sec) >= 1000u) {
            garden_policy_tick(&policy);
            last_sec = now_ms;
        }
        push_mask();
    }
    garden_policy_all_off(&policy);
    push_mask();
    release_all();
    printf("GARDEN_APP stopped\n");
}
