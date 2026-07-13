# Safe service startup and master clock recovery

## Startup lifecycle

`initVariant()` now performs only work that must happen before the normal Arduino `setup()`:

- initialize the persisted Heroku synchronization default
- load the persisted Master/Slave role
- select the Slave recovery AP subnet when required
- register role-aware routes before compatibility routes
- arm a passive post-setup gate

The gate does not operate relays, WiFi, schedules, heartbeats, or time services. It waits until the normal firmware setup has initialized configuration, GPIO, timezone, AP/station WiFi, NTP, weather, the HTTP server, and the core remote task.

After that point it starts:

- reliable slave schedule execution and run reporting
- slave Internet-time fallback
- slave schedule catch-up
- compatibility mesh heartbeat/timer service
- master clock recovery

Slave mode keeps Heroku synchronization disabled while setup is still running, even if the base configuration reloads an older enabled value.

## Master clock recovery

Master mode no longer relies only on the passive `configTzTime()` call.

When the clock is invalid it:

1. allows a short grace period for normal system SNTP;
2. sends a directly verified NTP request on the current station network;
3. tries the secondary NTP server if the primary fails;
4. temporarily joins the configured fallback Internet WiFi when the primary network cannot provide time;
5. returns to the normal station network after synchronization;
6. reports a critical clock fault through the existing mesh administration state when no valid time path is available.

## Missed local watering reconciliation

When an invalid master clock becomes valid, or a forward correction skips more than 90 seconds, the master examines local zone and spigot schedules from the previous 60 minutes.

Eligible occurrences are queued chronologically. Catch-up runs:

- are executed one at a time;
- wait for any current watering run to finish;
- allow an exact-minute regular schedule to take priority;
- wait rather than overlap a regular schedule that would begin during the catch-up duration;
- never start when the full run cannot finish by 20:00;
- revalidate the schedule before starting so edited or disabled rows are discarded;
- publish actual relay state after the catch-up run starts.

A clock that was already valid before setup does not trigger catch-up solely because of a soft reboot, reducing duplicate watering risk.
