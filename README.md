# Garden Controller (Heroku + Node.js)

API and authenticated GUI for managing an **ESP32-S3-Relay-6CH** controller.

## Deploy to Heroku

1. Create a Heroku app and set stack/buildpack for Node.js.
2. Set config vars:
   - `API_TOKEN`
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
API_TOKEN=change-me GUI_USERNAME=admin GUI_PASSWORD=change-me npm start
```

## API endpoints

All `/api/*` endpoints require header: `x-api-token: <API_TOKEN>`.

- `GET /health` - health check.
- `GET /api/relays` - current relay channel states.
- `GET /api/state` - current controller state payload:
  - relay states
  - command queue depth
  - server UTC time (`ISO-8601`)
  - schedules
- `POST /api/commands` - queue relay command.
  - body:
    ```json
    { "channel": 1, "action": "on", "requestedBy": "gui" }
    ```
- `GET /api/queue/next` - microcontroller polls next command; returns `204` when queue is empty.
- `GET /api/news` - list news feed items.
- `POST /api/news` - create a news feed item.
  - body:
    ```json
    { "title": "Watering Window", "body": "Relays 1-3 active tonight." }
    ```
- `GET /openapi.json` - OpenAPI spec rendered as JSON from `openapi.yaml`.

## GUI endpoints

- `GET /gui` - basic-auth protected management page showing:
  - current relay state
  - current server UTC time
  - schedules list
  - per-relay toggle controls
- `POST /gui/relays/:channel/toggle` - queue a toggle command from GUI and redirect to `/gui`.

Use `GUI_USERNAME` and `GUI_PASSWORD` as HTTP Basic credentials. If values are entered in Heroku with surrounding quotes, the app normalizes them automatically.

## Microcontroller polling flow

1. GUI/API client queues a command using `POST /api/commands` or GUI toggle control.
2. ESP32 calls `GET /api/queue/next` on interval.
3. API returns next command and updates internal relay state.
4. ESP32 executes relay change locally.

## CLI commands

```bash
npm start   # start web server
npm test    # run unit tests
```
