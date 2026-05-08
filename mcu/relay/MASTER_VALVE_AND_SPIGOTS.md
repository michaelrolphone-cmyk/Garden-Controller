# Master Valve + Spigots Design

## Hardware assignment

The 6-channel relay board is now used as:

```text
Relay channel 1 -> Zone 1 valve
Relay channel 2 -> Zone 2 valve
Relay channel 3 -> Zone 3 valve
Relay channel 4 -> Zone 4 valve
Relay channel 5 -> Zone 5 valve
Relay channel 6 -> Master valve / spigot supply
```

There are five scheduled irrigation zones. Relay channel 6 is not a scheduled zone.

## Master valve rule

The master valve relay is automatically controlled by firmware.

```text
master valve ON  = any zone relay is ON OR spigot run is active
master valve OFF = no zone relay is ON AND no spigot run is active
```

Scheduled zone runs:
1. turn off other zone valves,
2. turn on the selected zone valve,
3. automatically turn on relay channel 6/master valve,
4. turn off the selected zone at the end,
5. turn off master valve only if spigots are not also active.

## Spigot behavior

Spigots are supplied by the master valve. They are not scheduled zones.

Local endpoint:

```http
GET /api/spigots-run?minutes=30
```

Stop spigots:

```http
GET /api/spigots-run?action=off
```

Compatibility behavior:

```http
GET /api/manual-run?zone=6&minutes=30
```

also starts a timed spigot/master-valve run.

## Remote command behavior

Remote commands for channels 1-5 control zone valves.

Remote commands for channel 6 control the spigot/master-valve timed run:

```json
{
  "command": {
    "id": "server-command-id",
    "channel": 6,
    "action": "on",
    "durationSeconds": 1800
  }
}
```

Supported channel 6 actions:

```text
on     -> start spigot run, default 30 min if durationSeconds omitted
off    -> stop spigot run
toggle -> start if inactive, stop if active
```

## Telemetry

Relay telemetry still publishes six relay channels.

```json
{
  "relays": [
    { "channel": 1, "state": "off", "role": "zone", "zone": 1 },
    { "channel": 2, "state": "off", "role": "zone", "zone": 2 },
    { "channel": 3, "state": "off", "role": "zone", "zone": 3 },
    { "channel": 4, "state": "off", "role": "zone", "zone": 4 },
    { "channel": 5, "state": "off", "role": "zone", "zone": 5 },
    { "channel": 6, "state": "on", "role": "master_valve_spigots" }
  ],
  "masterValveChannel": 6,
  "masterValveOn": true,
  "spigotRun": {
    "active": true,
    "remainingSeconds": 1200
  }
}
```

Schedule telemetry publishes only zones 1-5.
