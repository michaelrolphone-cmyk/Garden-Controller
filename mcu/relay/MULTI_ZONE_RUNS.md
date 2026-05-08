# Multiple Simultaneous Zone Runs

v25 changes the run model from one active zone run to one active run timer per zone.

## Behavior

- Manual zone runs can overlap.
- Scheduled zone runs can overlap.
- If two schedule entries start at the same time, both zones turn on.
- If a second zone is started manually while another zone is running, the first zone remains on.
- Master valve relay channel 6 stays on while any zone run or spigot run is active.
- All Off still turns off every zone, the spigot run, and the master valve.

## Telemetry

Firmware now publishes:

```json
{
  "currentRun": {
    "active": true,
    "activeZoneCount": 2
  },
  "zoneRuns": [
    { "zone": 1, "channel": 1, "active": true, "remainingSeconds": 500 },
    { "zone": 2, "channel": 2, "active": true, "remainingSeconds": 300 }
  ]
}
```

The old single-zone `currentRun.zone` model is obsolete.
