# Garden Controller (Heroku + Node.js)

API and authenticated GUI for managing an **ESP32-S3-Relay-6CH** controller.

## Deploy to Heroku

1. Create a Heroku app and set stack/buildpack for Node.js.
2. Set config vars:
   - `API_KEY`
   - `GUI_USERNAME`
   - `GUI_PASSWORD`
3. Deploy this repo.

`Procfile` web dyno command:

```bash
web: node src/server.js
```

## Local run

```bash
npm install
API_KEY=change-me GUI_USERNAME=admin GUI_PASSWORD=change-me npm start
```

## Relay allocation

- Channels 1-5 are scheduled irrigation zones.
- Channel 6 is the master valve / spigot supply.
- Status RGB LED is a single WS2812B (NeoPixel) on GPIO 38 (serial data), not discrete R/G/B GPIO pins.
- The firmware automatically turns channel 6 on whenever a zone is running.
- Spigots and manual zone runs use timed commands (default 15 minutes).
- Schedules may only target channels 1-5.

## API endpoints

All `/api/*` endpoints require header: `x-api-token: <API_KEY>`.

- `GET /health` - health check.
- `GET /api/relays` - desired vs reported relay states and canonical `zoneColors` (hex + RGB) for zones 1-5:
  - `desiredRelays`: what API/GUI requested.
  - `reportedRelays`: what firmware last published.
- `GET /api/state` - current controller state payload:
  - desired and reported relay states
  - queued command depth (`status = queued`)
  - delivered command depth (`status = delivered`)
  - active long polls currently waiting (`pendingPolls`)
  - command history (acknowledged commands)
  - server UTC time (`ISO-8601`)
  - schedules
  - latest microcontroller telemetry snapshot
  - canonical `zoneColors` used by firmware RGB LED, firmware UI, and Node.js GUI
- `POST /api/commands` - queue relay command and update desired relay state.
  - body:
    ```json
    { "channel": 1, "action": "on", "requestedBy": "gui", "durationSeconds": 900 }
    ```
  - channel 6 timed run example:
    ```json
    { "channel": 6, "action": "on", "durationSeconds": 1800 }
    ```
- `POST /api/spigots/run` - queue a timed channel 6 spigot/master valve run.
- `POST /api/spigots/stop` - queue a channel 6 spigot/master valve stop command.
- `GET /api/queue/next` - microcontroller fetches next command.
  - supports optional query param `wait` (seconds, max `25`) for long-polling: `GET /api/queue/next?wait=25`
  - when queue is empty and `wait` is provided, server holds request until a command arrives or timeout returns `204`
  - response body (`200`):
    ```json
    {
      "command": {
        "id": "1715144970000-a1b2c3",
        "channel": 1,
        "action": "on",
        "requestedBy": "gui-web",
        "createdAt": "2026-05-08T09:00:00.000Z",
        "status": "delivered",
        "deliveredAt": "2026-05-08T09:00:01.100Z"
      }
    }
    ```
- `POST /api/microcontroller/commands/:id/ack` - firmware acknowledges delivered command status.
  - body:
    ```json
    { "status": "applied" }
    ```
  - valid status values: `applied`, `failed`.
- `POST /api/microcontroller/state` - canonical full telemetry publish endpoint.
  - body:
    ```json
    {
      "deviceId": "garden-relay-6",
      "firmwareVersion": "v12",
      "clockValid": true,
      "epoch": 1777901400,
      "localTime": "6:30am",
      "localDate": "Monday, May 4th",
      "homeWifiConnected": true,
      "homeIp": "192.168.1.42",
      "relays": [{ "channel": 1, "state": "on" }],
      "schedules": [{ "channel": 2, "zone": "Patio", "startTime": "06:45", "durationSeconds": 480 }],
      "currentRun": { "active": true, "activeZoneCount": 2 },
      "zoneRuns": [
        { "zone": 1, "channel": 1, "active": true, "remainingSeconds": 500 },
        { "zone": 2, "channel": 2, "active": true, "remainingSeconds": 300 }
      ],
      "lastCommandId": "1715144970000-a1b2c3"
    }
    ```
  - server fills telemetry `lastSeenAt`.
  - telemetry may include `targetLocation` and `sensorData` snapshots, supports `zoneRuns` for concurrent zone activity telemetry, and preserves firmware-provided clock/connectivity fields (`epoch`, `localTime`, `localDate`, `homeWifiConnected`, `homeIp`).
- `POST /api/microcontroller/sensors` - firmware publishes local sensor/device observations.
  - body:
    ```json
    {
      "deviceId": "garden-relay-6",
      "firmwareVersion": "v16-weather-sensor-baseline",
      "targetLocation": { "lat": 43.665288, "lon": -116.259186, "label": "garden" },
      "sensorData": [
        { "source": "relay-hardware", "type": "wifi_rssi", "value": -67, "unit": "dBm" }
      ]
    }
    ```
- `GET /api/sensors` - list recent sensor readings.
- `GET /api/sensors/latest` - latest sensor payload from firmware.
- `GET /api/weather/datasets` - all cached weather datasets.
- `GET /api/weather/current` - current NWS/grid weather dataset.
- `GET /api/weather/forecast` - NWS forecast/hourly dataset.
- `GET /api/weather/radar` - radar metadata/dataset.
- `GET /api/weather/satellite` - satellite metadata/dataset.
- `POST /api/weather/refresh` - force refresh external weather datasets.
- `POST /api/microcontroller/relays/state` - microcontroller publishes current relay on/off states.
- `POST /api/microcontroller/schedules` - microcontroller publishes current relay schedules.
- `POST /api/schedules` - operator/API publishes a full daily schedule list replacement and queues one `schedule_update` for the microcontroller (max 64 entries, channels 1-5, repeated channels allowed, supports an empty list to clear schedules).
- `DELETE /api/schedules/:id` - remove one scheduled time block by schedule `id` and queue a replacement `schedule_update` payload for firmware.
  - body:
    ```json
    {
      "schedules": [
        { "id": 0, "channel": 1, "zone": "Zone 1", "enabled": true, "startTime": "06:00", "durationSeconds": 600 },
        { "id": 1, "channel": 1, "zone": "Zone 1", "enabled": true, "startTime": "15:30", "durationSeconds": 420 }
      ],
      "requestedBy": "operator"
    }
    ```
- `GET /api/news` - list news feed items.
- `POST /api/news` - create a news feed item.
- `GET /openapi.json` - OpenAPI spec rendered as JSON from `openapi.yaml`.

## GUI endpoints

- `GET /gui` - basic-auth protected management page showing:
  - per-channel desired relay state and reported relay state
  - mismatch indicator when desired and reported differ
  - current server UTC time
  - client-side state refresh every 1 second for near-real-time visibility without reloading the full page
  - active zone highlighting on the garden map based on reported relay ON states
  - schedules list
  - embedded environmental monitoring panels for weather radar and satellite imagery centered at `43.665288, -116.259186`
  - live microcontroller weather sensor table (latest firmware-reported `sensorData`)
  - live device telemetry table with all canonical telemetry fields from `POST /api/microcontroller/state`
  - `lastSeenAt` plus relative "time since last telemetry update" for monitor-at-a-glance status
  - lidar/elevation quick link to USGS National Map (3DEP) centered at the garden coordinates
  - per-relay explicit ON/OFF controls (based on reported state)
- `POST /gui/relays/:channel/on` - queue a timed ON command from GUI (`minutes`, default 15) and redirect to `/gui`.
- `POST /gui/relays/:channel/off` - queue an OFF command from GUI and redirect to `/gui`.
- `POST /gui/schedules` - submit zone schedules from GUI (`durationMinutes`) and queue a schedule update command for the microcontroller; include `schedule[n][id]` to amend existing schedule entries instead of creating new identifiers. The GUI always submits the full schedule list (existing rows + newly added time slots) so firmware receives complete daily schedules on each save, and accepts an empty submission to clear all schedules.
- `POST /gui/schedules/:id/delete` - delete a single schedule row by `id` directly from the update rows form and queue a replacement full schedule update for firmware.
- `POST /gui/spigots/run` - queue a timed spigot run from GUI (`minutes`, default 15) and redirect to `/gui`.
- `POST /gui/spigots/stop` - queue a spigot stop command from GUI and redirect to `/gui`.
- `GET /gui/state` - basic-auth protected JSON state payload used by the GUI for 1-second incremental updates of relay/schedule/hardware status, including `latestSensorData`, full `deviceTelemetry`, and canonical `zoneColors`.

Use `GUI_USERNAME` and `GUI_PASSWORD` as HTTP Basic credentials. If values are entered in Heroku with surrounding quotes, the app normalizes them automatically.

## Microcontroller polling + acknowledgement flow

1. GUI/API client queues command using `POST /api/commands` or GUI explicit ON/OFF control (desired state updates immediately).
2. ESP32 opens `GET /api/queue/next?wait=25`.
3. API immediately returns the first queued command as `delivered`, or holds the request up to 25 seconds and returns `204` on timeout.
4. ESP32 executes relay/schedule action locally.
5. ESP32 immediately opens the next long-poll request.
6. ESP32 calls `POST /api/microcontroller/commands/:id/ack` with `applied` or `failed`.
7. Acknowledged commands are archived to `commandHistory` and removed from active queue.
8. ESP32 publishes reported relay and telemetry state via `POST /api/microcontroller/state` (or narrow endpoints).

## CLI commands

```bash
npm start   # start web server
npm test    # run unit tests
```


## Multiple daily schedules

Schedules are no longer one-per-zone.

The `schedules` array is a full daily schedule list. The same zone/channel may appear multiple times per day.

```json
{
  "schedules": [
    { "channel": 1, "zone": "Zone 1", "enabled": true, "startTime": "06:00", "durationSeconds": 600 },
    { "channel": 1, "zone": "Zone 1", "enabled": true, "startTime": "15:30", "durationSeconds": 420 },
    { "channel": 2, "zone": "Zone 2", "enabled": true, "startTime": "06:15", "durationSeconds": 600 }
  ]
}
```

`POST /api/schedules` replaces the firmware’s active daily schedule list.

## E-Ink zone map companion firmware

Firmware file:
- `mcu/relay/GardenEInkZoneDisplay.ino`

Hardware target:
- 7.5-inch 800x480 black/white e-paper using `GxEPD2_750_GDEY075T7`.
- Display pin map: MOSI 14, SCLK 13, CS 15, DC 27, RST 26, BUSY 25.
- SD pin map: CS 5, MISO 12.

E-paper controller APIs:
- `GET /`
- `GET /state`
- `GET /extra?zone=N&minutes=M` (validates `zone` 1-5 and `minutes` 1-240)
- `GET /stop`
- `GET /sync`
- `GET /redraw`
- `GET /saveZone?zone=N&name=...&baseMinutes=M&startHour=H&startMinute=M` (persists zone fields with bounds checks)
- `GET /saveLogic`
- `POST /saveNews`
- `GET /history.csv`
- `GET /clearHistory`
- `GET /display?mode=auto|schedule|news|graph`
- `GET /queue/clear`
- `GET /queue/stop-clear`
- `GET /ledger/reset`
- `GET /api/config`
- `POST /api/config`

Relay APIs consumed by e-paper firmware:
- `GET /time` (`epoch`, `synced`)
- `GET /weather` (`summary`, `condition`, `temperatureF`, `rainIn`, `windMph`, `windDeg`, `windDirection`, `humidityPct`, `dewPointF`, `precipitationChancePct`, `sunlightHours`, `predictedPrecipIn`, `sunriseEpoch`, `sunsetEpoch`, `weatherCode`, `lastWeatherMs`)
- `GET /status` (running state) with fallback to `GET /api/state`

State payload additions:
- `/state` includes `queueState`, `queueDepth`, `pendingExtraZone`, `pendingExtraMinutes`, and `soilLedger` metrics for dashboard/admin visibility.

Display behavior:
- Main schedule screen title: `Castle Hills Garden Watering Schedule`.
- Uses required layout coordinates:
  - `drawMap(8, 48, 424, 424)`
  - `drawWeatherWidget(432, 48, 360, 160)`
  - `drawSchedulePanel(432, 207, 360, 133)`
  - `drawRuntimePanel(432, 339, 360, 133)`
- Supports full-screen `Castle Hills Garden News` and `Current + Weekly Weather` screens.
- Auto-rotation cycle: 4 minutes with watering-active suppression to schedule screen.
- Garden map uses zone polygons with active-zone hatch + inverted label badges for readability.
- Weather widget includes compass-style wind gauge and sunrise/sunset strip.
- Full refresh on substantial layout/screen changes; partial refresh for runtime meter updates.

CLI commands:
```bash
npm start
npm test -- --runTestsByPath test/eink-firmware.test.js
```
