# Day-of-week watering schedules

The relay firmware supports an independent seven-day mask for each automatic schedule entry. Existing schedules default to all seven days, so upgrading does not disable any current watering plan.

## Day bit mapping

The mask follows the ESP32 `tm_wday` order:

| Day | Bit | Value |
|---|---:|---:|
| Sunday | 0 | 1 |
| Monday | 1 | 2 |
| Tuesday | 2 | 4 |
| Wednesday | 3 | 8 |
| Thursday | 4 | 16 |
| Friday | 5 | 32 |
| Saturday | 6 | 64 |

Common values:

- All days: `127`
- Weekdays: `62`
- Weekends: `65`
- No automatic days: `0`

## Captive-portal admin interface

Connect to the relay access point and open the existing administration page:

```text
http://192.168.4.1/admin
```

The existing **Schedule Manager** now contains Sunday-through-Saturday checkboxes inside every schedule row. It also provides per-row presets for all days, weekdays, weekends, and no days.

The existing **Save Schedules** button saves all of the following together from the operator's perspective:

- Zone
- Start time
- Duration
- Enabled state
- Watering days

The previous `/schedule-days` page is no longer a separate editor. Requests to it redirect to `/admin#schedule-manager`.

Manual zone runs and timed spigot runs are not restricted by the automatic-schedule weekday masks.

The masks are persisted in the ESP32 `Preferences` namespace `relay6days`. Schedule signatures retain masks when rows are reordered or deleted. A new or materially changed row defaults to all days until the admin save operation assigns its selected mask.

## JSON API

Read masks:

```http
GET /api/schedule-days
```

Update masks by schedule ID:

```http
POST /api/schedule-days
Content-Type: application/json

{
  "schedules": [
    { "id": 0, "daysMask": 62 },
    { "id": 1, "days": "Mon|Wed|Fri" },
    { "id": 2, "days": ["Tue", "Thu"] }
  ]
}
```

The API accepts either `daysMask`, a day-name string, or an array of day names.

## Source layout

`GardenSimpleRelay6Core.inc` remains the unchanged firmware baseline from `0.1.0-BETA`. `GardenSimpleRelay6.ino` wraps that core, restores the existing initialization and route set, replaces the automatic schedule check with weekday-aware execution, and injects the weekday controls into the existing captive-portal admin page.
