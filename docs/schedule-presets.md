# Schedule presets

Weather watering presets are stored in `data/schedule-presets.json` and are loaded by the Heroku web process at GUI render time.

## How the preset schema works

Each preset can define either a full `schedules` array or a compact `passes` array. The deployed presets currently use `passes`.

A pass expands across zones 1-5 unless a custom `zones` array is provided.

```json
{
  "startTime": "05:30",
  "durationMinutes": 22,
  "offsetMinutes": 30
}
```

That example expands to:

- Zone 1 at 05:30 for 22 minutes
- Zone 2 at 06:00 for 22 minutes
- Zone 3 at 06:30 for 22 minutes
- Zone 4 at 07:00 for 22 minutes
- Zone 5 at 07:30 for 22 minutes

## Validation rules

The app validates both preset application and custom schedule saves:

- Zones must be channels 1-5.
- Enabled schedule rows must start no earlier than 04:00.
- Enabled schedule rows must finish no later than 20:00.
- Enabled zones may not overlap each other.
- The schedule list may not exceed the firmware maximum of 64 entries.

Back-to-back runs are allowed. For example, a 5-minute Zone 1 run at 14:00 and a 5-minute Zone 2 run at 14:05 are valid.

## Included presets

- Current Baseline
- High Summer Heat
- Germination / Seedlings
- Rainy / Skip Watering
- Cold Weather
- Fall Reduced
- Frost Threat Flash
- Transplant Establishment
- Wind / Low Humidity

## GUI behavior

The `/gui` page shows a weather preset selector above the existing custom schedule editor. Selecting **Apply preset** submits the selected preset to the existing `/gui/schedules` route, so the firmware still receives a normal full daily schedule update.

The custom schedule editor remains available for manual schedule editing.
