# Scheduled spigot support

Relay channel 6 is the master-valve/spigot circuit. The firmware can now schedule this channel independently, using the same start time, duration, enabled state, and weekday controls used for Zones 1-5.

## Captive-portal administration

Connect to the relay access point and open:

```text
http://192.168.4.1/admin
```

In **Schedule Manager**, select **Spigots** in the Valve field. Each row supports:

- Start time
- Duration in minutes
- Enabled or disabled state
- Sunday-through-Saturday selection
- All days, Weekdays, Weekends, and No days shortcuts

The existing **Save Schedules** button saves zone and spigot rows together.

## Runtime behavior

A scheduled Spigots row calls the existing timed spigot-run mechanism. It energizes channel 6 without opening any zone valve and turns off after its configured duration.

Channel 6 remains the master valve for Zones 1-5. The firmware calculates master-valve demand as:

```text
any zone relay is on OR a timed spigot run is active
```

Therefore, expiration or cancellation of a spigot run cannot turn channel 6 off while a zone still requires the master valve.

## Persistence and synchronization

Spigot schedules are stored in ESP32 Preferences under `relay6spig`. This is separate from the existing zone schedule storage, preserving backward compatibility with installed schedule data.

The firmware publishes combined zone and spigot schedules to Heroku. It also synchronizes channel-6 schedules from:

```http
GET /api/firmware/spigot-schedules
```

A locally saved captive-portal schedule is marked dirty until it has been successfully published, so an offline local edit is not silently overwritten by the server copy.

## Schedule constraints

The Heroku scheduler accepts channels 1-6 and enforces the existing rules:

- No more than 64 total schedule entries
- Enabled runs must start no earlier than 04:00
- Enabled runs must finish no later than 20:00
- Enabled zone and spigot runs may not overlap

## API representation

A spigot schedule uses `channel: 6` and the label `Spigots`:

```json
{
  "id": 12,
  "channel": 6,
  "zone": "Spigots",
  "enabled": true,
  "startTime": "07:00",
  "durationSeconds": 900,
  "daysMask": 62
}
```

`daysMask: 62` means Monday through Friday. Existing day-name strings and day arrays remain supported by the firmware weekday API.
