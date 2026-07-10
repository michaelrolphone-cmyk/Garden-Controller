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

## Firmware editor

Open the relay's local page:

```text
http://192.168.4.1/schedule-days
```

The page lists every schedule with Sunday-through-Saturday checkboxes. Saving changes affects automatic schedule starts only. Manual zone runs and timed spigot runs are unchanged.

The masks are persisted in the ESP32 `Preferences` namespace `relay6days`. Schedule signatures are used to retain masks when rows are reordered or deleted. A new or materially changed row defaults to all days.

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

`GardenSimpleRelay6Core.inc` is the unchanged firmware baseline from `0.1.0-BETA`. `GardenSimpleRelay6.ino` includes that core and replaces only the top-level `setup`, `loop`, and automatic schedule-check call. This keeps all existing relay, weather, remote API, display, and manual-run behavior intact while adding weekday filtering.
