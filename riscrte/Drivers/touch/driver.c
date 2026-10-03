/* Event fan-out adapted from Reader gt911_touch at 3d9bc4f; CST816D transport below. */
#include "RiscTouchV1.h"
#include "RiscI2cBusV1.h"
#include "RiscPlatformClockV1.h"
#include <stddef.h>
#include <stdint.h>
#include "../common/gpio.h"
#include <stdatomic.h>

#define GT911_PRIMARY_ADDRESS 0x5du
#define GT911_FALLBACK_ADDRESS 0x14u
#define GT911_PRODUCT_ID_REG 0x8140u
#define GT911_STATUS_REG 0x814eu
#define GT911_FIRST_POINT_REG 0x814fu
#define GT911_READY_MASK 0x80u
#define GT911_TOUCH_COUNT_MASK 0x0fu
#define GT911_HAVE_KEY_MASK 0x10u
#define GT911_TIMEOUT_MS 20u


typedef struct {
    uint64_t token;
    risc_touch_event_v1 queue[RISC_TOUCH_QUEUE_LENGTH];
    uint8_t head;
    uint8_t count;
    bool gap;
} touch_subscriber;

/* Public API calls can originate from a capture task and an app. The bus
 * mutex protects one transfer, not this complete status/read/ack/state cycle.
 * Lifecycle start/stop remains serialized by the grant-owning loader. */
static atomic_flag state_lock = ATOMIC_FLAG_INIT;
static bool lock_state(void) { return !atomic_flag_test_and_set_explicit(&state_lock,memory_order_acquire); }
static void unlock_state(void) { atomic_flag_clear_explicit(&state_lock,memory_order_release); }

static const risc_i2c_bus_api_v1 *bus;
static const risc_platform_clock_api_v1 *clock_api;
static uint64_t bus_claim;
static uint64_t sequence;
static uint64_t subscription_serial;
static uint16_t surface_width;
static uint16_t surface_height;
static risc_touch_contact_v1 contacts[RISC_TOUCH_MAX_CONTACTS];
static uint8_t contact_count;
static uint32_t touch_buttons;
static uint64_t snapshot_timestamp_ms;
static touch_subscriber subscribers[RISC_TOUCH_MAX_SUBSCRIBERS];

static inline bool equal(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static uint64_t monotonic_ms(void) {
    if (!clock_api || !clock_api->monotonic_ms) return UINT64_MAX;
    return clock_api->monotonic_ms(clock_api->context);
}

static bool read_reg(uint8_t reg, uint8_t *out, size_t length) {
    return bus && bus_claim && bus->transact(bus->context,bus_claim,&reg,1,out,length,20);
}

static int find_contact(const risc_touch_contact_v1 *items, uint8_t count,
                        uint8_t id) {
    for (uint8_t i = 0; i < count; ++i)
        if (items[i].id == id) return (int)i;
    return -1;
}

static bool emit(uint8_t kind, uint8_t id, uint16_t x, uint16_t y,
                 uint64_t timestamp_ms) {
    if (sequence == UINT64_MAX) return false;
    risc_touch_event_v1 event = {0};
    event.sequence = ++sequence;
    event.timestamp_ms = timestamp_ms;
    event.kind = kind;
    event.id = id;
    event.x = x;
    event.y = y;

    for (uint8_t i = 0; i < RISC_TOUCH_MAX_SUBSCRIBERS; ++i) {
        touch_subscriber *subscriber = &subscribers[i];
        if (!subscriber->token || subscriber->gap) continue;
        if (subscriber->count == RISC_TOUCH_QUEUE_LENGTH) {
            subscriber->gap = true;
            subscriber->head = 0;
            subscriber->count = 0;
            continue;
        }
        const uint8_t tail =
            (uint8_t)((subscriber->head + subscriber->count) %
                      RISC_TOUCH_QUEUE_LENGTH);
        subscriber->queue[tail] = event;
        ++subscriber->count;
    }
    return true;
}

static bool apply_state(const risc_touch_contact_v1 *next,
                        uint8_t next_count, uint32_t next_buttons,
                        uint64_t timestamp_ms) {
    for (uint8_t i = 0; i < contact_count; ++i) {
        if (find_contact(next, next_count, contacts[i].id) < 0 &&
            !emit(RISC_TOUCH_EVENT_UP, contacts[i].id,
                  contacts[i].x, contacts[i].y, timestamp_ms))
            return false;
    }

    for (uint8_t i = 0; i < next_count; ++i) {
        const int old_index = find_contact(contacts, contact_count, next[i].id);
        if (old_index < 0) {
            if (!emit(RISC_TOUCH_EVENT_DOWN, next[i].id,
                      next[i].x, next[i].y, timestamp_ms))
                return false;
        } else {
            const risc_touch_contact_v1 *old = &contacts[old_index];
            if ((old->x != next[i].x || old->y != next[i].y) &&
                !emit(RISC_TOUCH_EVENT_MOVE, next[i].id,
                      next[i].x, next[i].y, timestamp_ms))
                return false;
        }
    }

    if ((touch_buttons & RISC_TOUCH_BUTTON_PRIMARY) &&
        !(next_buttons & RISC_TOUCH_BUTTON_PRIMARY) &&
        !emit(RISC_TOUCH_EVENT_BUTTON_UP, 0u, 0u, 0u, timestamp_ms))
        return false;
    if (!(touch_buttons & RISC_TOUCH_BUTTON_PRIMARY) &&
        (next_buttons & RISC_TOUCH_BUTTON_PRIMARY) &&
        !emit(RISC_TOUCH_EVENT_BUTTON_DOWN, 0u, 0u, 0u, timestamp_ms))
        return false;

    for (uint8_t i = 0; i < next_count; ++i) contacts[i] = next[i];
    for (uint8_t i = next_count; i < RISC_TOUCH_MAX_CONTACTS; ++i)
        contacts[i] = (risc_touch_contact_v1){0};
    contact_count = next_count;
    touch_buttons = next_buttons;
    snapshot_timestamp_ms = timestamp_ms;
    return true;
}

/* Discarding an invalid hardware report is a real stream discontinuity.
 * Tell every subscriber instead of allowing a later UP to fabricate a tap. */
static void invalidate_subscribers(void) {
    for (uint8_t i = 0; i < RISC_TOUCH_MAX_SUBSCRIBERS; ++i) {
        if (!subscribers[i].token) continue;
        subscribers[i].gap = true;
        subscribers[i].head = subscribers[i].count = 0;
    }
}

/* CST816D report: touch count at 0x02 followed by 12-bit X/Y. */
static bool service_one(bool *had_report) {
    uint8_t raw[5];
    if (had_report) *had_report=false;
    if (!read_reg(0x02,raw,sizeof(raw))) return false;
    uint8_t count=raw[0]&15;
    if (count>1) { invalidate_subscribers(); return false; }
    risc_touch_contact_v1 next={0};
    next.x=((raw[1]&15)<<8)|raw[2]; next.y=((raw[3]&15)<<8)|raw[4];
    if (count && (next.x>=surface_width || next.y>=surface_height)) { invalidate_subscribers(); return false; }
    if (had_report) *had_report=true;
    return apply_state(&next,count,0,monotonic_ms());
}

static uint64_t subscribe_locked(void *context) {
    (void)context;
    if (!bus || !bus_claim || subscription_serial == UINT64_MAX) return 0;
    for (uint8_t i = 0; i < RISC_TOUCH_MAX_SUBSCRIBERS; ++i) {
        if (!subscribers[i].token) {
            subscribers[i] = (touch_subscriber){0};
            subscribers[i].token = ++subscription_serial;
            return subscribers[i].token;
        }
    }
    return 0;
}

static bool unsubscribe_locked(void *context, uint64_t token) {
    (void)context;
    if (!token) return false;
    for (uint8_t i = 0; i < RISC_TOUCH_MAX_SUBSCRIBERS; ++i) {
        if (subscribers[i].token == token) {
            subscribers[i] = (touch_subscriber){0};
            return true;
        }
    }
    return false;
}

static bool poll_locked(void *context, size_t max_reports) {
    (void)context;
    if (!bus || !clock_api || !bus_claim || !max_reports || max_reports > 16u)
        return false;
    /* One complete report per call: at most three 20 ms bus operations.
     * The API promises up to max_reports, not a mandatory full batch. Avoid
     * monopolizing the provider/bus when a producer continuously asserts READY. */
    bool had_report = false;
    return service_one(&had_report);
}

static int32_t next_event_locked(void *context, uint64_t token,
                          risc_touch_event_v1 *out) {
    (void)context;
    if (!bus || !bus_claim || !token || !out) return -1;
    for (uint8_t i = 0; i < RISC_TOUCH_MAX_SUBSCRIBERS; ++i) {
        touch_subscriber *subscriber = &subscribers[i];
        if (subscriber->token != token) continue;
        if (subscriber->gap) {
            subscriber->gap = false;
            subscriber->head = 0;
            subscriber->count = 0;
            return -1;
        }
        if (!subscriber->count) return 0;
        *out = subscriber->queue[subscriber->head];
        subscriber->head =
            (uint8_t)((subscriber->head + 1u) % RISC_TOUCH_QUEUE_LENGTH);
        --subscriber->count;
        return 1;
    }
    return -1;
}

static bool snapshot_locked(void *context, risc_touch_snapshot_v1 *out) {
    (void)context;
    if (!bus || !bus_claim || !out) return false;
    *out = (risc_touch_snapshot_v1){0};
    out->sequence = sequence;
    out->timestamp_ms = snapshot_timestamp_ms;
    out->width = surface_width;
    out->height = surface_height;
    out->contact_count = contact_count;
    out->buttons = touch_buttons;
    for (uint8_t i = 0; i < contact_count; ++i) out->contacts[i] = contacts[i];
    return true;
}

static bool start(const risc_provider_dependency_v1 *d,size_t n) {
    if (bus || bus_claim || !gpio_dependencies(d,n) || garden_board(d,n)!=GARDEN_BOARD_DIAL) return false;
    const risc_i2c_bus_api_v1 *candidate=garden_dependency(d,n,"i2c.bus",sizeof(*candidate));
    if (!candidate || !candidate->claim_device || !candidate->transact || !candidate->release_device) return false;
    bus=candidate; clock_api=timer;
    if (!gpio_output(13) || !gpio_input(5)) return false;
    gpio_write(13,false); timer->sleep_ms(timer->context,10);
    gpio_write(13,true); timer->sleep_ms(timer->context,50);
    if (io_fault || !bus->claim_device(bus->context,0x15,&bus_claim) || !bus_claim) return false;
    surface_width=surface_height=240; contact_count=0; touch_buttons=0;
    snapshot_timestamp_ms=monotonic_ms();
    for (size_t i=0;i<RISC_TOUCH_MAX_SUBSCRIBERS;i++) subscribers[i]=(touch_subscriber){0};
    bool report=false; return service_one(&report);
}

static bool quiesce_locked(void) {
    if (!bus && !clock_api && !bus_claim) return true;
    for (uint8_t i = 0; i < RISC_TOUCH_MAX_SUBSCRIBERS; ++i)
        if (subscribers[i].token) return false;
    if (bus_claim && (!bus || !bus->release_device(bus->context, bus_claim)))
        return false;
    bus_claim = 0;
    gpio_release(5); gpio_release(13);
    if (!gpio_clean()) return false;
    bus = NULL;
    clock_api = NULL;
    contact_count = 0;
    touch_buttons = 0;
    surface_width = surface_height = 0;
    return true;
}

static uint64_t subscribe(void *context) {
    if (!lock_state()) return 0;
    const uint64_t result = subscribe_locked(context);
    unlock_state();
    return result;
}
static bool unsubscribe(void *context, uint64_t token) {
    if (!lock_state()) return false;
    const bool result = unsubscribe_locked(context, token);
    unlock_state();
    return result;
}
static bool poll(void *context, size_t max_reports) {
    if (!lock_state()) return false;
    const bool result = poll_locked(context, max_reports);
    unlock_state();
    return result;
}
static int32_t next_event(void *context, uint64_t token, risc_touch_event_v1 *out) {
    if (!lock_state()) return -2;
    const int32_t result = next_event_locked(context, token, out);
    unlock_state();
    return result;
}
static bool snapshot(void *context, risc_touch_snapshot_v1 *out) {
    if (!lock_state()) return false;
    const bool result = snapshot_locked(context, out);
    unlock_state();
    return result;
}
static bool quiesce(void) {
    if (!lock_state()) return false;
    const bool result = quiesce_locked();
    unlock_state();
    return result;
}
static void stop(void) {
    if (!quiesce()) return;
    /* The loader calls stop only after the last grant and API caller drained. */

}

static const risc_touch_api_v1 touch_api = {
    RISC_TOUCH_API_V1, sizeof(risc_touch_api_v1), NULL,
    subscribe, unsubscribe, poll, next_event, snapshot
};

static const risc_driver_v2 driver = {
    RISC_PROVIDER_DRIVER_ABI_V2, sizeof(risc_driver_v2),
    "touch", "input.touch.raw", RISC_TOUCH_API_V1,
    &touch_api, start, stop, quiesce
};

__attribute__((visibility("default")))
const risc_driver_v2 *t5_driver_get(uint32_t abi) {
    return abi == RISC_PROVIDER_DRIVER_ABI_V2 ? &driver : NULL;
}
