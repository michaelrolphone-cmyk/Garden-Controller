/* Garden Controller RiscRTE application.
 * Relay and buzzer are generic capabilities. Zone policy stays in this app. */
#include "../../include/garden_policy.h"
#include "../Drivers/buzzer/BuzzerApi.h"
#include "../Drivers/garden_encoder/GardenEncoderApi.h"
#include "../Drivers/relay/RelayApi.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t buttons; bool exit_requested; } t5_app_input_t;
#define T5_APP_BUTTON_BACK 1u
typedef struct {
    bool (*poll)(t5_app_input_t *input, uint32_t timeout_ms);
    uint32_t (*millis)(void);
    uint32_t (*micros)(void);
} t5_app_api_v1;
static garden_policy_t policy;
static uint8_t selected_zone = 1;
static const relay_api_v1 *relay;
static const buzzer_api_v1 *buzzer;
static const garden_encoder_api_v1 *encoder;
static const t5_app_api_v1 *app;
static void push_mask(void) {
    if (relay && relay->set_mask) relay->set_mask(relay->context, garden_policy_relay_mask(&policy));
}
static void indicate(void) {
    if (buzzer && buzzer->chirp) buzzer->chirp(buzzer->context, 0);
}
void garden_app_bind(const relay_api_v1 *relay_api, const buzzer_api_v1 *buzzer_api, const garden_encoder_api_v1 *encoder_api, const t5_app_api_v1 *app_api) {
    relay = relay_api; buzzer = buzzer_api; encoder = encoder_api; app = app_api;
}
void garden_app_on_minute(uint8_t hour, uint8_t minute) {
    garden_policy_apply_due(&policy, hour, minute);
    push_mask();
}
void garden_app_step(uint32_t now_us, bool one_second) {
    if (encoder && encoder->poll) encoder->poll(encoder->context, now_us);
    if (encoder && encoder->take_detents) {
        int32_t d = encoder->take_detents(encoder->context);
        while (d > 0) { selected_zone = (uint8_t)(selected_zone % GARDEN_ZONE_COUNT + 1); d--; }
        while (d < 0) { selected_zone = selected_zone == 1 ? GARDEN_ZONE_COUNT : (uint8_t)(selected_zone - 1); d++; }
    }
    if (encoder && encoder->take_long_press && encoder->take_long_press(encoder->context)) {
        garden_policy_all_off(&policy);
        indicate();
    } else if (encoder && encoder->take_click && encoder->take_click(encoder->context)) {
        if (policy.zone_on[selected_zone - 1]) garden_policy_stop_zone(&policy, selected_zone);
        else {
            garden_policy_start_zone(&policy, selected_zone, GARDEN_DEFAULT_RUN_SEC);
            indicate();
        }
    }
    if (one_second) garden_policy_tick(&policy);
    push_mask();
}
uint8_t garden_app_selected_zone(void) { return selected_zone; }
uint8_t garden_app_mask(void) { return garden_policy_relay_mask(&policy); }
void app_main(void) {
    garden_policy_init(&policy);
    if (!relay || !buzzer || !encoder || !app || !app->poll || !app->millis) return;
    uint32_t last_sec = app->millis();
    for (;;) {
        t5_app_input_t input;
        memset(&input, 0, sizeof(input));
        if (!app->poll(&input, 20) || input.exit_requested || (input.buttons & T5_APP_BUTTON_BACK)) break;
        uint32_t now_ms = app->millis();
        bool second = (uint32_t)(now_ms - last_sec) >= 1000u;
        if (second) last_sec = now_ms;
        garden_app_step(app->micros ? app->micros() : now_ms * 1000u, second);
    }
    garden_policy_all_off(&policy);
    push_mask();
    if (buzzer && buzzer->set) buzzer->set(buzzer->context, 0, false);
}
