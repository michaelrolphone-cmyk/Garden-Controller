# Weather Data Integration Baseline

Garden coordinates:

```text
43.665288, -116.259186
```

## Division of responsibility

The relay firmware publishes local device/sensor observations.

The Heroku server should fetch/cache internet weather datasets:

- current/forecast weather from NWS
- NEXRAD/MRMS radar metadata or tile links
- GOES/GEOSS satellite imagery metadata or tile links

The ESP32 should not scrape or process NEXRAD/GOES satellite data directly.

## Firmware sensor payload

Firmware posts local sensor/device data to:

```http
POST /api/microcontroller/sensors
```

Payload shape:

```json
{
  "deviceId": "garden-relay-6",
  "firmwareVersion": "v16-weather-sensor-baseline",
  "targetLocation": {
    "lat": 43.665288,
    "lon": -116.259186,
    "label": "garden"
  },
  "sensorData": [
    {
      "source": "relay-hardware",
      "type": "wifi_rssi",
      "value": -67,
      "unit": "dBm",
      "observedAtEpoch": 1777901400
    },
    {
      "source": "relay-hardware",
      "type": "weather_sensor_placeholder",
      "temperatureC": null,
      "relativeHumidityPct": null,
      "precipitationMm": null,
      "solarIrradianceWm2": null,
      "windSpeedMps": null,
      "note": "No local weather sensors installed yet; schema reserved for future hardware sensors."
    }
  ]
}
```

The same `targetLocation` and `sensorData` arrays are also included in the canonical telemetry payload:

```http
POST /api/microcontroller/state
```

## Recommended server endpoints

Add:

```text
POST /api/microcontroller/sensors
GET  /api/sensors
GET  /api/weather/current
GET  /api/weather/forecast
GET  /api/weather/radar
GET  /api/weather/satellite
GET  /api/weather/datasets
POST /api/weather/refresh
```

## Server-side internet weather collection

For the target coordinates, server should resolve NWS grid metadata using:

```text
GET https://api.weather.gov/points/43.665288,-116.259186
```

Then follow the returned `forecast`, `forecastHourly`, and `forecastGridData` links.

For NEXRAD/radar and GOES satellite data, the server should store dataset metadata and visualization URLs/tile templates, not raw image blobs in memory.


## v17 / PR #20 note

The canonical telemetry endpoint now has server support for preserving device clock and WiFi connectivity fields:

```text
epoch
localTime
localDate
homeWifiConnected
homeIp
```

The firmware sends these fields in every `POST /api/microcontroller/state` payload.


## v18 garden timezone note

The relay firmware uses the garden coordinates to set the fixed garden timezone:

```text
America/Boise
MST7MDT,M3.2.0,M11.1.0
```

All watering schedules are interpreted in garden local time.


## v19 configurable location/timezone note

Location and timezone are no longer hard-coded runtime behavior. Defaults exist for first boot, but active values are persisted and configurable.

The server should resolve timezone from coordinates and push both:
- IANA timezone name
- ESP32 POSIX timezone rule

The firmware uses the POSIX timezone string for `getLocalTime()` and schedule execution.
