# Safe service startup and master clock recovery

## Startup lifecycle

`initVariant()` performs only work that must happen before the normal Arduino `setup()`:

- initialize the persisted Heroku synchronization default
- load the persisted Master/Slave role
- select the Slave recovery AP subnet when required
- register role-aware routes before compatibility routes
- arm a passive post-setup gate

The gate does not operate relays, WiFi, schedules, heartbeats, or time services. It waits until the normal firmware setup has initialized configuration, GPIO, timezone, AP/station WiFi, NTP, weather, the HTTP server, and the core remote task.

After that point it starts:

- reliable slave schedule distribution and run reporting
- slave Internet-time fallback
- compatibility mesh heartbeat/timer service
- master clock recovery
- absolute schedule-interval reconciliation
- time-fallback maintenance

Slave mode keeps Heroku synchronization disabled while setup is still running, even if the base configuration reloads an older enabled value.

## Master clock recovery

Master mode does not rely only on the passive `configTzTime()` call.

When the clock is invalid it:

1. allows a short grace period for normal system SNTP;
2. sends a directly verified NTP request on the current station network;
3. tries the secondary NTP server if the primary fails;
4. temporarily joins the configured fallback Internet WiFi when the primary network cannot provide time;
5. returns to the normal station network after synchronization;
6. reports a critical clock fault through the existing mesh administration state when no valid time path is available.

## Absolute interval reconciliation

A schedule is treated as a fixed local-time interval:

```text
scheduled start <= current time < scheduled end
```

During every reconciliation pass:

- a relay whose schedule interval contains the current time is kept on;
- its timer is set only to the remaining time before the original scheduled end;
- a schedule-controlled relay is turned off when the interval ends;
- reevaluating the same interval is idempotent and does not extend watering;
- a clock that becomes valid during an interval immediately restores the relay for only the remaining portion;
- an interval that has already ended is not replayed later;
- local zone, local spigot, and slave-zone schedules use the same rule.

The previous delayed full-duration catch-up queues were removed.
