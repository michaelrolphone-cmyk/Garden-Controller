#pragma once
/* Zone policy shared by the RiscRTE garden app. Relays 1-5 are zones.
 * Relay 6 is the master valve / spigots and is never a scheduled zone.
 * Matches GardenSimpleRelay6 v20-v25 field behavior. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GARDEN_ZONE_COUNT 5u
#define GARDEN_RELAY_COUNT 6u
#define GARDEN_MASTER_INDEX 5u
#define GARDEN_SCHEDULE_CAP 64u
#define GARDEN_DEFAULT_RUN_SEC 900u

typedef struct {
    uint8_t zone;          /* 1..5 */
    uint8_t hour;          /* 0..23 garden local */
    uint8_t minute;        /* 0..59 */
    uint16_t duration_sec;
    bool enabled;
} garden_schedule_t;

typedef struct {
    bool zone_on[GARDEN_ZONE_COUNT];
    uint32_t remain_sec[GARDEN_ZONE_COUNT];
    bool master_on;
    bool spigot_on;
    uint32_t spigot_remain_sec;
    garden_schedule_t schedules[GARDEN_SCHEDULE_CAP];
    uint8_t schedule_count;
    uint8_t last_fired_hour;
    uint8_t last_fired_minute;
    bool have_fired_minute;
} garden_policy_t;

void garden_policy_init(garden_policy_t *p);
bool garden_policy_add_schedule(garden_policy_t *p, garden_schedule_t entry);
void garden_policy_clear_schedules(garden_policy_t *p);
bool garden_policy_start_zone(garden_policy_t *p, uint8_t zone, uint32_t seconds);
void garden_policy_stop_zone(garden_policy_t *p, uint8_t zone);
bool garden_policy_start_spigot(garden_policy_t *p, uint32_t seconds);
void garden_policy_stop_spigot(garden_policy_t *p);
void garden_policy_all_off(garden_policy_t *p);
void garden_policy_tick(garden_policy_t *p);
uint8_t garden_policy_apply_due(garden_policy_t *p, uint8_t hour, uint8_t minute);
uint8_t garden_policy_relay_mask(const garden_policy_t *p);
bool garden_policy_any_zone(const garden_policy_t *p);
