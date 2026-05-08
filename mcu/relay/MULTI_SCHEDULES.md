# Multiple Daily Schedules

v23 supports multiple schedule entries per day.

## Model

There are still five valve zones:

```text
Zone channels: 1-5
Relay 6: master valve/spigots
```

Schedules are now stored as independent daily entries:

```json
{
  "id": 0,
  "channel": 1,
  "zone": "Zone 1",
  "enabled": true,
  "startTime": "06:00",
  "durationSeconds": 600
}
```

The same zone can appear more than once per day, for example:

```json
[
  { "channel": 1, "zone": "Zone 1", "enabled": true, "startTime": "06:00", "durationSeconds": 600 },
  { "channel": 1, "zone": "Zone 1", "enabled": true, "startTime": "15:30", "durationSeconds": 420 }
]
```

## Capacity

The firmware stores up to 64 schedule entries in ESP32 Preferences.

This is a firmware/local-storage limit, not a server API limit. The server can keep a larger schedule library later, but should push a daily active schedule list capped to the firmware capacity.

## Local admin format

The local `/admin` page uses one schedule line per run:

```text
zone,HH:MM,minutes,on/off
```

Examples:

```text
1,06:00,10,on
1,15:30,7,on
2,06:10,12,on
5,16:00,8,on
```

## Server schedule_update

The firmware accepts a `schedule_update` command with an array of schedule entries. It clears the existing daily schedule list and stores the received list up to the 64-entry firmware limit.
