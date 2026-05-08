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

## API endpoints

All `/api/*` endpoints require header: `x-api-token: <API_KEY>`.

- `GET /health` - health check.
- `GET /api/relays` - desired vs reported relay states:
  - `desiredRelays`: what API/GUI requested.
  - `reportedRelays`: what firmware last published.
- `GET /api/state` - current controller state payload:
  - desired and reported relay states
  - queued command depth (`status = queued`)
  - command history (acknowledged commands)
  - server UTC time (`ISO-8601`)
  - schedules
- `POST /api/commands` - queue relay command and update desired relay state.
  - body:
    ```json
    { "channel": 1, "action": "on", "requestedBy": "gui" }
    ```
- `GET /api/queue/next` - microcontroller polls next command; returns `204` when queue is empty.
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
      "relays": [{ "channel": 1, "state": "on" }],
      "schedules": [{ "channel": 2, "zone": "Patio", "startTime": "06:45", "durationSeconds": 480 }],
      "currentRun": { "channel": 2, "remainingSeconds": 120 },
      "lastCommandId": "1715144970000-a1b2c3"
    }
    ```
  - server fills telemetry `lastSeenAt`.
- `POST /api/microcontroller/relays/state` - microcontroller publishes current relay on/off states.
- `POST /api/microcontroller/schedules` - microcontroller publishes current relay schedules.
- `POST /api/schedules` - operator/API publishes schedule updates and queues them for the microcontroller.
  - body:
    ```json
    {
      "schedules": [
        { "channel": 4, "zone": "Greenhouse", "startTime": "07:00", "durationSeconds": 300 }
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
  - schedules list
  - per-relay explicit ON/OFF controls (based on reported state)
- `POST /gui/relays/:channel/on` - queue an ON command from GUI and redirect to `/gui`.
- `POST /gui/relays/:channel/off` - queue an OFF command from GUI and redirect to `/gui`.
- `POST /gui/schedules` - submit a zone schedule from GUI and queue a schedule update command for the microcontroller.

Use `GUI_USERNAME` and `GUI_PASSWORD` as HTTP Basic credentials. If values are entered in Heroku with surrounding quotes, the app normalizes them automatically.

## Microcontroller polling + acknowledgement flow

1. GUI/API client queues command using `POST /api/commands` or GUI explicit ON/OFF control (desired state updates immediately).
2. ESP32 calls `GET /api/queue/next` on interval.
3. API marks first queued command as `delivered` and returns it.
4. ESP32 executes relay/schedule action locally.
5. ESP32 calls `POST /api/microcontroller/commands/:id/ack` with `applied` or `failed`.
6. ESP32 publishes reported relay and telemetry state via `POST /api/microcontroller/state` (or narrow endpoints).

## CLI commands

```bash
npm start   # start web server
npm test    # run unit tests
```
