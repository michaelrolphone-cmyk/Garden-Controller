# GardenSimpleRelay6 v12

Compile-fixed full simple 6-channel firmware.

## What was fixed

- Restored missing local web UI handlers:
  - `handleRoot`
  - `handleAdmin`
  - `handleSaveAdmin`
  - `sendStateJson`
  - `handleSetTime`
  - `handleRelay`
  - `handleManualRun`
  - `handleAllOff`
  - `handleBuzzerTest`
  - `handleFactoryReset`
  - `setupAp`

- Fixed bad timer declaration.
- Kept API `channel` semantics as user-visible zone number.
- Uses updated Garden-Controller API:
  - `POST /api/microcontroller/relays/state`
  - `POST /api/microcontroller/schedules`
  - `GET /api/queue/next`
- Admin schedule uses one `type="time"` field per zone.
- AP + STA WiFi mode remains active.


## v14 New Server API integration

Updated firmware to match the new Garden-Controller API baseline without removing the local GUI/admin firmware code.

Added:
- canonical `POST /api/microcontroller/state`
- command acknowledgement to `POST /api/microcontroller/commands/:id/ack`
- queue command handling for delivered command objects
- schedule update acknowledgement
- firmware version and last-command reporting
- local `/api/remote/test` no longer consumes `/api/queue/next`

Preserved:
- mobile GUI at `/`
- admin GUI at `/admin`
- local schedule editor using `type="time"`
- AP + STA WiFi behavior
- manual run / all off / buzzer test / factory reset handlers


## v15 long-poll firmware update

Updated firmware for PR #11 server long-poll support.

Changed:
- command polling now uses `GET /api/queue/next?wait=25`
- GET timeout increased to 30 seconds
- POST timeout set to 10 seconds
- remote API work now runs in a background FreeRTOS task
- main `loop()` no longer blocks on remote HTTP
- full telemetry publishes every 60 seconds while connected
- command long-poll resumes immediately after 200 or 204

Preserved:
- mobile GUI at `/`
- admin GUI at `/admin`
- local schedule editor with `type="time"`
- AP + STA WiFi behavior
- manual run
- all off
- buzzer test
- factory reset
- command ack flow
- canonical telemetry publish


## v16 weather/sensor baseline

Added garden target coordinates and local sensor-data publication baseline.

Garden target:

```text
43.665288, -116.259186
```

Firmware now includes in canonical telemetry:
- `targetLocation`
- `sensorData`

Firmware also posts sensor/device observations to:

```text
POST /api/microcontroller/sensors
```

Current relay-board observations:
- WiFi RSSI
- uptime
- free heap
- placeholder weather-sensor object for future physical sensors

The ESP32 does not fetch NEXRAD/GOES/weather web data. The server should fetch/cache those internet datasets for the garden coordinates and expose them to the GUI/analysis layer.

See `GardenSimpleRelay6/WEATHER_DATA_INTEGRATION.md`.


## v17 PR20 full telemetry alignment

Updated firmware version and documentation for Garden-Controller PR #20.

The firmware's canonical telemetry publish already includes the fields PR #20 now preserves server-side:

- `epoch`
- `localTime`
- `localDate`
- `homeWifiConnected`
- `homeIp`
- `targetLocation`
- `sensorData`
- `currentRun`
- `lastCommandId`

No local GUI/admin code was removed.

Preserved:
- mobile GUI at `/`
- admin GUI at `/admin`
- long-poll command handling
- command acknowledgement
- weather/sensor baseline
- AP + STA WiFi
- manual run / all off / buzzer / factory reset


## v18 garden timezone scheduling

Schedules are now explicitly interpreted in the garden's local timezone:

```text
Garden location: 43.665288, -116.259186
Timezone: America/Boise
ESP32 POSIX TZ: MST7MDT,M3.2.0,M11.1.0
```

Firmware now:
- sets the garden timezone on startup before schedule logic runs
- uses `configTzTime(GARDEN_POSIX_TZ, ...)`
- reports `timeZone` and `posixTimeZone` in telemetry
- labels local UI time as garden time
- keeps schedule `startTime` semantics as garden local time

No local GUI/admin code was removed.


## v19 configurable location/timezone

Corrected v18 hard-coding. The garden location/timezone is now configurable and persisted.

Defaults remain for first boot/factory reset only:

```text
43.665288, -116.259186
America/Boise
MST7MDT,M3.2.0,M11.1.0
```

Runtime behavior now uses persisted/configured values:
- `gardenLatitude`
- `gardenLongitude`
- `gardenTimeZone`
- `gardenPosixTimeZone`

Local admin now exposes fields to edit:
- garden latitude
- garden longitude
- IANA timezone
- ESP32 POSIX timezone

Remote commands now support:
- `location_update`
- `config_update`
- optional location/timezone fields inside `schedule_update`

The server should resolve timezone from location and send both the IANA name and ESP32 POSIX TZ string. The ESP32 uses the POSIX string for schedule execution.


## v20 master valve + 5 zones

Updated relay allocation:

```text
Relay 1-5 = irrigation zones
Relay 6   = master valve / spigots
```

Changed firmware behavior:
- Zone schedules only run zones 1-5.
- Relay channel 6 is no longer a scheduled zone.
- Master valve automatically turns on whenever any zone is on.
- Master valve automatically turns off when no zone and no spigot run is active.
- Spigots can be run manually for a timed duration.
- `/api/spigots-run?minutes=30` starts a spigot run.
- `/api/spigots-run?action=off` stops spigots.
- `/api/manual-run?zone=6&minutes=30` also starts spigots for compatibility.
- Remote command channel 6 controls spigots/master-valve timed run.

Preserved:
- mobile GUI
- admin GUI
- long-poll command handling
- command acknowledgement
- configurable location/timezone
- weather/sensor telemetry
- AP + STA WiFi
- all off / buzzer / factory reset


## v21 buzzer volume reduction

Reduced passive buzzer drive to about 1/10 of prior volume by changing the chirp pulse train from approximately 50% duty to 5% duty.

```cpp
static const uint8_t BUZZER_DUTY_PERCENT = 5;
```

No relay, schedule, GUI, master valve, spigot, long-poll, weather, or timezone behavior was changed.


## v22 timed zone commands

Updated firmware for Garden-Controller PR #22.

Changed:
- server zone ON commands now start timed manual runs using `durationSeconds`
- default manual run duration is 900 seconds / 15 minutes
- zone OFF commands stop the active zone/current run
- zone TOGGLE commands stop if active, otherwise start a timed run
- channel 6 spigot/master valve commands also use `durationSeconds`
- local prompts now default to 15 minutes

Preserved:
- 5 zones + relay 6 master valve/spigots
- quieter buzzer
- mobile GUI
- admin GUI
- long-poll command handling
- command acknowledgement
- configurable location/timezone
- weather/sensor telemetry
- AP + STA WiFi
- all off / factory reset


## v23 multiple schedules per day

Updated firmware to support multiple schedule entries per day.

Changed:
- zones 1-5 can each have multiple daily schedule entries
- morning and afternoon watering for the same zone is now supported
- server `schedule_update` can send repeated channels
- local admin schedule editor uses one line per schedule:
  `zone,HH:MM,minutes,on/off`
- firmware stores up to 64 daily schedule entries in ESP32 Preferences
- telemetry `schedules` publishes the full daily schedule list

Preserved:
- 5 zones + relay 6 master valve/spigots
- timed server zone commands
- 1/10 buzzer volume
- mobile GUI
- admin GUI
- long-poll command handling
- command acknowledgement
- configurable location/timezone
- weather/sensor telemetry
- AP + STA WiFi
- all off / factory reset


## v24 fast remote polling

Updated remote API timing.

Changed:
- command polling is now continuous long-poll
- firmware immediately opens the next `/api/queue/next?wait=25` request after a command or 204 response
- `remoteIntervalSeconds` no longer throttles command checks
- full telemetry publishes every 15 seconds instead of 60 seconds
- sensor telemetry remains at 5 minutes

Preserved:
- multiple daily schedules
- 5 zones + relay 6 master valve/spigots
- timed server zone commands
- 1/10 buzzer volume
- mobile GUI
- admin GUI
- command acknowledgement
- configurable location/timezone
- weather/sensor telemetry
- AP + STA WiFi
- all off / factory reset


## v25 multiple simultaneous zone runs

Updated firmware to support overlapping manual and scheduled zone runs.

Changed:
- each of the 5 zones now has its own run timer
- starting one zone no longer shuts off other running zones
- schedule checker no longer refuses to start a zone while another zone is active
- all schedule entries due in the same minute are started
- telemetry now includes `zoneRuns` with per-zone active/remaining time details
- `currentRun` remains as a summary with `active` and `activeZoneCount`

Preserved:
- multiple daily schedules
- 5 zones + relay 6 master valve/spigots
- timed server zone commands
- 1/10 buzzer volume
- mobile GUI
- admin GUI
- continuous long-poll command handling
- command acknowledgement
- configurable location/timezone
- weather/sensor telemetry
- AP + STA WiFi
- all off / factory reset


## v25 compile fix

Restored the configuration and daily schedule helper functions that were missing from the previous v25 build:
- `clearDailySchedules`
- `addDailySchedule`
- `scheduleLine`
- `parseScheduleText`
- `defaultConfig`
- `saveConfig`
- `loadConfig`
- `factoryReset`

Also added explicit forward declarations so the Arduino builder does not fail prototype generation.

## v26 local API expansion for admin/mobile clients

Firmware now exposes local API resources so local clients can use API calls directly for telemetry, configuration, schedules, relay state control, and feature discovery over Wi-Fi. The firmware admin UI now includes direct zone ON/OFF controls and an API-backed schedule manager that posts full schedule payloads to local endpoints.

Added endpoints:
- `GET /api/features` - discover available telemetry and controls.
- `GET /api/config` - read current editable local configuration.
- `POST /api/config` - update local configuration using JSON.
- `POST /api/schedules` - replace schedules from JSON array payload.

Existing control/telemetry endpoints retained for local clients:
- `GET /api/state` (includes runtime telemetry plus editable local config fields used by `/admin` hydration: `apSsid`, `apPass`, `staSsid`, `staPass`, `remoteEnabled`, `remoteApiBase`, `remoteDeviceId`, `remoteApiKey`, `gardenLatitude`, `gardenLongitude`, `gardenTimeZone`, `gardenPosixTimeZone`)
- `GET /api/time/set?epoch=...`
- `GET /api/relay?zone=1&state=1`
- `GET /api/manual-run?zone=1&minutes=15`
- `GET /api/spigots-run?minutes=15`
- `GET /api/spigots-run?action=off`
- `GET /api/alloff`
- `GET /api/buzzer-test`
- `GET /api/factory-reset`

This keeps local API access unauthenticated on the LAN/AP as requested.

## v27 admin UI aligned to server web app layout

The firmware `/admin` page now mirrors the server web interface structure for local-only operation:
- "Castle Hills Garden Manager" style admin shell
- Garden zone map with active-zone highlighting
- Zone cards with run/stop/timed-run controls
- Schedule timeline rendering from firmware local state
- 1-second refresh loop from `GET /api/state`

This admin UI uses firmware local endpoints directly over Wi-Fi and does not depend on server-pushed payloads.
