#include <T5AppApi.h>
#include <T5ProviderCapabilityApi.h>
#include "../Drivers/button/ButtonApi.h"
#include "../Drivers/garden_encoder/GardenEncoderApi.h"
#include <stdio.h>
#include <string.h>

static const t5_app_api_v1 *app;
static const t5_provider_capability_api_v1 *providers;
static const garden_encoder_api_v1 *encoder;
static const button_api_v1 *button;
static t5_provider_capability_lease_t encoder_lease;
static t5_provider_capability_lease_t button_lease;

static bool lease_one(const char *capability, uint32_t version, t5_provider_capability_lease_t *lease, const void **out) {
    *lease = T5_PROVIDER_CAPABILITY_LEASE_INVALID;
    *out = NULL;
    if (!providers->acquire(capability, version, lease, out) || !*out) {
        printf("GARDEN_ENCODER missing %s\n", capability);
        return false;
    }
    return true;
}

void app_main(void) {
    app = t5_app_get_api(T5_APP_ABI_VERSION);
    providers = t5_provider_capability_get_api(T5_PROVIDER_CAPABILITY_API_VERSION);
    if (!app || !app->poll || !app->millis || !providers || !providers->acquire || !providers->release) {
        printf("GARDEN_ENCODER unavailable-api\n");
        return;
    }
    const void *borrowed = NULL;
    if (!lease_one("input.quadrature", GARDEN_ENCODER_API_V1, &encoder_lease, &borrowed)) return;
    encoder = borrowed;
    if (!lease_one("input.button", BUTTON_API_V1, &button_lease, &borrowed)) {
        providers->release(encoder_lease);
        return;
    }
    button = borrowed;
    if (!encoder->poll || !encoder->take_detents || !button->poll || !button->take_click) {
        providers->release(encoder_lease);
        providers->release(button_lease);
        return;
    }
    uint8_t zone = 1;
    bool running = false;
    printf("GARDEN_ENCODER zone=%u run=0\n", zone);
    for (;;) {
        t5_app_input_t input;
        memset(&input, 0, sizeof(input));
        if (!app->poll(&input, 20) || input.exit_requested || (input.buttons & T5_APP_BUTTON_BACK)) break;
        uint32_t now_us = app->millis() * 1000u;
        encoder->poll(encoder->context, now_us);
        button->poll(button->context, now_us);
        int32_t detents = encoder->take_detents(encoder->context);
        uint8_t next = zone;
        while (detents > 0) { next = (uint8_t)(next % 6 + 1); detents--; }
        while (detents < 0) { next = next == 1 ? 6 : (uint8_t)(next - 1); detents++; }
        bool next_running = running;
        if (button->take_click(button->context, 0)) next_running = !next_running;
        if (next != zone || next_running != running) {
            zone = next;
            running = next_running;
            printf("GARDEN_ENCODER zone=%u run=%u\n", zone, running ? 1u : 0u);
        }
    }
    providers->release(encoder_lease);
    providers->release(button_lease);
    printf("GARDEN_ENCODER stopped\n");
}
