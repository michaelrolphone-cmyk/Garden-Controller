#include "garden_policy.h"

static void recompute_master(garden_policy_t *p) {
    p->master_on = garden_policy_any_zone(p) || p->spigot_on;
}

void garden_policy_init(garden_policy_t *p) {
    if (!p) return;
    p->master_on = false;
    p->spigot_on = false;
    p->spigot_remain_sec = 0;
    p->schedule_count = 0;
    p->last_fired_hour = 0;
    p->last_fired_minute = 0;
    p->have_fired_minute = false;
    for (uint8_t i = 0; i < GARDEN_ZONE_COUNT; i++) {
        p->zone_on[i] = false;
        p->remain_sec[i] = 0;
    }
}

bool garden_policy_add_schedule(garden_policy_t *p, garden_schedule_t entry) {
    if (!p || p->schedule_count >= GARDEN_SCHEDULE_CAP) return false;
    if (entry.zone < 1 || entry.zone > GARDEN_ZONE_COUNT) return false;
    if (entry.hour > 23 || entry.minute > 59) return false;
    if (entry.duration_sec == 0 || entry.duration_sec > 8u * 3600u) return false;
    p->schedules[p->schedule_count++] = entry;
    return true;
}

void garden_policy_clear_schedules(garden_policy_t *p) {
    if (p) p->schedule_count = 0;
}

bool garden_policy_any_zone(const garden_policy_t *p) {
    if (!p) return false;
    for (uint8_t i = 0; i < GARDEN_ZONE_COUNT; i++) if (p->zone_on[i]) return true;
    return false;
}

bool garden_policy_start_zone(garden_policy_t *p, uint8_t zone, uint32_t seconds) {
    if (!p || zone < 1 || zone > GARDEN_ZONE_COUNT) return false;
    if (seconds == 0) seconds = GARDEN_DEFAULT_RUN_SEC;
    if (seconds > 8u * 3600u) seconds = 8u * 3600u;
    p->zone_on[zone - 1] = true;
    p->remain_sec[zone - 1] = seconds;
    recompute_master(p);
    return true;
}

void garden_policy_stop_zone(garden_policy_t *p, uint8_t zone) {
    if (!p || zone < 1 || zone > GARDEN_ZONE_COUNT) return;
    p->zone_on[zone - 1] = false;
    p->remain_sec[zone - 1] = 0;
    recompute_master(p);
}

bool garden_policy_start_spigot(garden_policy_t *p, uint32_t seconds) {
    if (!p) return false;
    if (seconds == 0) seconds = GARDEN_DEFAULT_RUN_SEC;
    if (seconds > 8u * 3600u) seconds = 8u * 3600u;
    p->spigot_on = true;
    p->spigot_remain_sec = seconds;
    recompute_master(p);
    return true;
}

void garden_policy_stop_spigot(garden_policy_t *p) {
    if (!p) return;
    p->spigot_on = false;
    p->spigot_remain_sec = 0;
    recompute_master(p);
}

void garden_policy_all_off(garden_policy_t *p) {
    if (!p) return;
    for (uint8_t i = 0; i < GARDEN_ZONE_COUNT; i++) {
        p->zone_on[i] = false;
        p->remain_sec[i] = 0;
    }
    p->spigot_on = false;
    p->spigot_remain_sec = 0;
    p->master_on = false;
}

void garden_policy_tick(garden_policy_t *p) {
    if (!p) return;
    for (uint8_t i = 0; i < GARDEN_ZONE_COUNT; i++) {
        if (!p->zone_on[i]) continue;
        if (p->remain_sec[i] <= 1) {
            p->zone_on[i] = false;
            p->remain_sec[i] = 0;
        } else {
            p->remain_sec[i]--;
        }
    }
    if (p->spigot_on) {
        if (p->spigot_remain_sec <= 1) {
            p->spigot_on = false;
            p->spigot_remain_sec = 0;
        } else {
            p->spigot_remain_sec--;
        }
    }
    recompute_master(p);
}

uint8_t garden_policy_apply_due(garden_policy_t *p, uint8_t hour, uint8_t minute) {
    if (!p || hour > 23 || minute > 59) return 0;
    if (p->have_fired_minute && p->last_fired_hour == hour && p->last_fired_minute == minute) return 0;
    uint8_t started = 0;
    for (uint8_t i = 0; i < p->schedule_count; i++) {
        const garden_schedule_t *s = &p->schedules[i];
        if (!s->enabled || s->hour != hour || s->minute != minute) continue;
        if (garden_policy_start_zone(p, s->zone, s->duration_sec)) started++;
    }
    p->have_fired_minute = true;
    p->last_fired_hour = hour;
    p->last_fired_minute = minute;
    return started;
}

uint8_t garden_policy_relay_mask(const garden_policy_t *p) {
    if (!p) return 0;
    uint8_t mask = 0;
    for (uint8_t i = 0; i < GARDEN_ZONE_COUNT; i++) if (p->zone_on[i]) mask |= (uint8_t)(1u << i);
    if (p->spigot_on || p->master_on) mask |= (uint8_t)(1u << GARDEN_MASTER_INDEX);
    return mask;
}
