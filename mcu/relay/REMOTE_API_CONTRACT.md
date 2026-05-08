# GardenSimpleRelay6 v19 Configurable Location + Timezone Contract

The device no longer treats the garden location/timezone as hard-coded runtime behavior.

Defaults are still compiled for first boot/factory reset:

```text
lat/lon: 43.665288, -116.259186
IANA timezone: America/Boise
ESP32 POSIX TZ: MST7MDT,M3.2.0,M11.1.0
```

But the active values are configurable and persisted in ESP32 Preferences.

## Important rule

The ESP32 cannot reliably infer an arbitrary IANA timezone from coordinates by itself. The server should resolve timezone from coordinates and send:

```json
{
  "targetLocation": { "lat": 43.665288, "lon": -116.259186, "label": "garden" },
  "timeZone": "America/Boise",
  "posixTimeZone": "MST7MDT,M3.2.0,M11.1.0"
}
```

The firmware then stores those values and uses `posixTimeZone` for schedule execution.

## Schedule semantics

Schedule `startTime` is interpreted in the configured garden timezone.

Example:

```json
{
  "type": "schedule_update",
  "targetLocation": { "lat": 43.665288, "lon": -116.259186 },
  "timeZone": "America/Boise",
  "posixTimeZone": "MST7MDT,M3.2.0,M11.1.0",
  "schedules": [
    { "channel": 1, "zone": "Zone 1", "startTime": "06:30", "durationSeconds": 600 }
  ]
}
```

Firmware behavior:

```text
06:30 = 6:30am in configured garden local time
```

## Supported remote config commands

The firmware accepts queued commands from `/api/queue/next?wait=25`:

```json
{
  "command": {
    "id": "server-command-id",
    "type": "location_update",
    "targetLocation": { "lat": 43.665288, "lon": -116.259186 },
    "timeZone": "America/Boise",
    "posixTimeZone": "MST7MDT,M3.2.0,M11.1.0"
  }
}
```

or:

```json
{
  "command": {
    "id": "server-command-id",
    "type": "config_update",
    "targetLocation": { "lat": 43.665288, "lon": -116.259186 },
    "timeZone": "America/Boise",
    "posixTimeZone": "MST7MDT,M3.2.0,M11.1.0"
  }
}
```

The firmware also honors `targetLocation`, `timeZone`, and `posixTimeZone` when included with a `schedule_update`.

## Canonical telemetry

Firmware reports the active configured values:

```json
{
  "targetLocation": {
    "lat": 43.665288,
    "lon": -116.259186,
    "label": "garden"
  },
  "timeZone": "America/Boise",
  "posixTimeZone": "MST7MDT,M3.2.0,M11.1.0"
}
```

## Local admin

The local `/admin` page exposes editable fields for:

```text
Garden latitude
Garden longitude
Garden IANA timezone
ESP32 POSIX timezone
```


# v20 Master Valve / 5 Zone Update

Firmware now treats relay channel 6 as the master valve / spigot supply.

## Channel semantics

```text
Channels 1-5 = scheduled irrigation zones
Channel 6    = master valve / spigots
```

Schedules only apply to channels 1-5.

Relay state telemetry still reports all 6 relay channels so the server can see the master valve state.

## Remote commands

Channels 1-5:

```json
{ "channel": 1, "action": "on" }
```

Channel 6:

```json
{ "channel": 6, "action": "on", "durationSeconds": 1800 }
```

Channel 6 command rules:
- `on` starts a timed spigot/master-valve run.
- `off` stops the spigot run.
- `toggle` starts/stops the spigot run.
- omitted `durationSeconds` defaults to 30 minutes.

## Server update needed

The server GUI/API should stop treating channel 6 as a zone.

It should display:
- 5 scheduled zones
- a separate master valve/spigots control
- relay channel 6 state as master valve state


# v22 Timed Zone Commands / PR #22

Garden-Controller PR #22 changed `POST /api/commands` so zone ON commands include `durationSeconds`, defaulting to 900 seconds / 15 minutes.

Firmware now handles zone commands this way:

```json
{
  "command": {
    "id": "server-command-id",
    "channel": 3,
    "action": "on",
    "durationSeconds": 900
  }
}
```

Firmware behavior:

```text
channel 1-5 + action on     -> timed manual zone run for durationSeconds
channel 1-5 + action off    -> stop that zone and clear current run if it matches
channel 1-5 + action toggle -> stop if on, otherwise timed run
channel 6   + action on     -> timed spigot/master-valve run for durationSeconds
```

The firmware owns the timer. The server should not queue a later OFF command to end a timed run.

If `durationSeconds` is omitted, firmware defaults to 900 seconds.


# v23 Multiple Daily Schedules

The firmware now supports multiple schedule entries per day.

Server `schedule_update` command can send repeated channels:

```json
{
  "command": {
    "id": "server-command-id",
    "type": "schedule_update",
    "schedules": [
      { "channel": 1, "zone": "Zone 1", "enabled": true, "startTime": "06:00", "durationSeconds": 600 },
      { "channel": 1, "zone": "Zone 1", "enabled": true, "startTime": "15:30", "durationSeconds": 420 },
      { "channel": 2, "zone": "Zone 2", "enabled": true, "startTime": "06:15", "durationSeconds": 600 }
    ]
  }
}
```

Firmware replaces the local daily schedule list with the received list.

Telemetry publishes all active schedule entries in the `schedules` array and local `/api/state` includes `dailySchedules`.


# v24 Fast Remote Polling

Remote command polling is now continuous long-poll:

```http
GET /api/queue/next?wait=25
```

The firmware no longer waits for `remoteIntervalSeconds` before opening the next command poll.

Full state telemetry frequency:

```text
15 seconds
```

Sensor telemetry frequency:

```text
300 seconds
```

The local `/api/state` exposes:

```json
{
  "commandPollingMode": "continuous_long_poll",
  "telemetryIntervalMs": 15000,
  "sensorIntervalMs": 300000
}
```


# v25 Multiple Simultaneous Zone Runs

Firmware now supports overlapping zone runs.

Server command behavior remains the same:

```json
{
  "command": {
    "channel": 1,
    "action": "on",
    "durationSeconds": 900
  }
}
```

But firmware no longer turns other zones off when one zone starts.

Schedule behavior:
- multiple schedule entries can share the same `startTime`
- overlapping schedules are allowed
- every due schedule entry for the current minute is started
- the firmware keeps one timer per zone

Telemetry includes a `zoneRuns` array in addition to `currentRun.active` and `currentRun.activeZoneCount`.
