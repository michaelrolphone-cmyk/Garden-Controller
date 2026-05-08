# Fast Remote Command Polling

v24 changes remote API timing so server commands are received quickly while still using weak-WiFi-friendly long polling.

## Command lane

The firmware continuously calls:

```http
GET /api/queue/next?wait=25
```

The server holds the request up to 25 seconds and responds immediately if a command is queued.

The firmware no longer waits 15 seconds between command polls. After either:
- `200` with command
- `204` no command

it opens the next long-poll request immediately.

## State telemetry lane

Full state telemetry now publishes every 15 seconds:

```http
POST /api/microcontroller/state
```

This was reduced from 60 seconds.

## Sensor lane

Sensor/device observations remain slower:

```http
POST /api/microcontroller/sensors
```

Current interval remains 300 seconds / 5 minutes.

## Timing constants

```cpp
REMOTE_LONG_POLL_WAIT_SECONDS = 25
REMOTE_GET_TIMEOUT_MS = 30000
REMOTE_POST_TIMEOUT_MS = 10000
REMOTE_TELEMETRY_INTERVAL_MS = 15000
REMOTE_SENSOR_INTERVAL_MS = 300000
```

## Important behavior

`remoteIntervalSeconds` no longer throttles command polling. It is left in the config for compatibility, but command polling is now continuous long-poll.
