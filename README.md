# Garden Controller (Heroku + Node.js)

API and simple authenticated GUI for managing an **ESP32-S3-Relay-6CH** controller.

## Deploy to Heroku

1. Create a Heroku app and set stack/buildpack for Node.js.
2. Set config vars:
   - `API_TOKEN`
   - `GUI_USERNAME`
   - `GUI_PASSWORD`
3. Deploy this repo.

`Procfile` is configured for web dyno startup:

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
- `GET /api/relays` - current 6 channel relay states.
- `POST /api/commands` - queue a relay command.
  - body:
    ```json
    { "channel": 1, "action": "on", "requestedBy": "gui" }
    ```
- `GET /api/queue/next` - microcontroller polls next command; returns `204` if queue is empty.
- `GET /api/news` - list news feed items.
- `POST /api/news` - create a news feed item.
  - body:
    ```json
    { "title": "Watering Window", "body": "Relays 1-3 active tonight." }
    ```
- `GET /openapi.json` - OpenAPI spec rendered as JSON from `openapi.yaml`.

## GUI endpoint

- `GET /gui` - basic-auth protected public management page.
  - use `GUI_USERNAME` and `GUI_PASSWORD` as HTTP Basic credentials.

## Microcontroller polling flow

1. GUI client queues a command using `POST /api/commands`.
2. ESP32 calls `GET /api/queue/next` on interval.
3. API returns next command and updates internal relay state.
4. ESP32 executes relay change locally.

## CLI commands

```bash
npm start   # start web server
npm test    # run unit tests
```
