const express = require('express');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');

const RELAY_CHANNELS = 6;

function normalizeCredentialValue(value) {
  if (typeof value !== 'string') {
    return value;
  }

  const trimmed = value.trim();
  const wrappedInQuotes =
    (trimmed.startsWith('\"') && trimmed.endsWith('\"')) ||
    (trimmed.startsWith("'") && trimmed.endsWith("'"));

  return wrappedInQuotes ? trimmed.slice(1, -1).trim() : trimmed;
}

function createState() {
  return {
    desiredRelayState: Array.from({ length: RELAY_CHANNELS }, (_, index) => ({ channel: index + 1, state: 'off' })),
    reportedRelayState: Array.from({ length: RELAY_CHANNELS }, (_, index) => ({ channel: index + 1, state: 'off' })),
    queue: [],
    commandHistory: [],
    news: [],
    schedules: [],
    deviceTelemetry: null
  };
}

function createCommand({ channel, action, requestedBy }) {
  return {
    id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
    channel,
    action,
    requestedBy: requestedBy || 'gui',
    createdAt: new Date().toISOString(),
    status: 'queued'
  };
}

function formatScheduleLabel(schedule) {
  if (typeof schedule === 'string') {
    return schedule;
  }

  return `${schedule.zone} (relay ${schedule.channel}) at ${schedule.startTime} for ${schedule.durationSeconds}s`;
}

function createApp(config = {}) {
  const app = express();
  app.use(express.json());
  app.use(express.urlencoded({ extended: false }));

  const state = config.state || createState();
  const pendingPolls = [];
  const MAX_LONG_POLL_WAIT_MS = 25_000;
  const guiUsername = normalizeCredentialValue(config.guiUsername ?? process.env.GUI_USERNAME);
  const guiPassword = normalizeCredentialValue(config.guiPassword ?? process.env.GUI_PASSWORD);
  const apiToken = normalizeCredentialValue(config.apiKey ?? config.apiToken ?? process.env.API_KEY ?? process.env.API_TOKEN);

  function getNextQueuedCommand() {
    return state.queue.find((queuedCommand) => queuedCommand.status === 'queued');
  }

  function deliverCommand(command) {
    command.status = 'delivered';
    command.deliveredAt = new Date().toISOString();
    return { command };
  }

  function removePendingPoll(poll) {
    const index = pendingPolls.indexOf(poll);
    if (index >= 0) {
      pendingPolls.splice(index, 1);
    }
  }

  function wakePendingPolls() {
    while (pendingPolls.length > 0) {
      const command = getNextQueuedCommand();
      if (!command) {
        return false;
      }

      const poll = pendingPolls.shift();
      clearTimeout(poll.timeout);
      poll.res.json(deliverCommand(command));
      return true;
    }

    return false;
  }

  function holdLongPoll(req, res) {
    const requestedWaitSeconds = Number.parseInt(req.query.wait, 10);
    const waitMs = Number.isInteger(requestedWaitSeconds)
      ? Math.max(1000, Math.min(requestedWaitSeconds * 1000, MAX_LONG_POLL_WAIT_MS))
      : 0;

    if (waitMs <= 0) {
      return res.status(204).send();
    }

    const poll = { req, res, timeout: null };

    poll.timeout = setTimeout(() => {
      removePendingPoll(poll);
      if (!res.headersSent) {
        res.status(204).send();
      }
    }, waitMs);

    req.on('close', () => {
      clearTimeout(poll.timeout);
      removePendingPoll(poll);
    });

    pendingPolls.push(poll);
    return undefined;
  }

  function requireApiToken(req, res, next) {
    const provided = req.get('x-api-token');
    if (!apiToken || provided !== apiToken) {
      return res.status(401).json({ error: 'Unauthorized' });
    }
    return next();
  }

  function requireGuiAuth(req, res, next) {
    const auth = req.get('authorization');
    if (!auth || !/^Basic\s+/i.test(auth)) {
      res.set('WWW-Authenticate', 'Basic realm="Garden Controller"');
      return res.status(401).send('Authentication required');
    }

    const encodedCredentials = auth.replace(/^Basic\s+/i, '');

    let decoded;
    try {
      decoded = Buffer.from(encodedCredentials, 'base64').toString('utf8');
    } catch (_error) {
      res.set('WWW-Authenticate', 'Basic realm="Garden Controller"');
      return res.status(401).send('Invalid credentials');
    }

    const separatorIndex = decoded.indexOf(':');
    const username = separatorIndex >= 0 ? decoded.slice(0, separatorIndex) : '';
    const password = separatorIndex >= 0 ? decoded.slice(separatorIndex + 1) : '';

    if (!guiUsername || !guiPassword || username !== guiUsername || password !== guiPassword) {
      res.set('WWW-Authenticate', 'Basic realm="Garden Controller"');
      return res.status(401).send('Invalid credentials');
    }

    return next();
  }

  app.get('/health', (_req, res) => {
    res.json({ ok: true });
  });

  app.get('/api/relays', requireApiToken, (_req, res) => {
    res.json({ desiredRelays: state.desiredRelayState, reportedRelays: state.reportedRelayState });
  });

  app.get('/api/state', requireApiToken, (_req, res) => {
    res.json({
      desiredRelays: state.desiredRelayState,
      reportedRelays: state.reportedRelayState,
      queueDepth: state.queue.filter((command) => command.status === 'queued').length,
      deliveredDepth: state.queue.filter((command) => command.status === 'delivered').length,
      pendingPolls: pendingPolls.length,
      commandHistory: state.commandHistory,
      serverTime: new Date().toISOString(),
      schedules: state.schedules,
      deviceTelemetry: state.deviceTelemetry
    });
  });

  app.post('/api/microcontroller/relays/state', requireApiToken, (req, res) => {
    const { relays } = req.body;
    const validRelayPayload =
      Array.isArray(relays) &&
      relays.length > 0 &&
      relays.every(
        (relay) =>
          relay &&
          Number.isInteger(relay.channel) &&
          relay.channel >= 1 &&
          relay.channel <= RELAY_CHANNELS &&
          (relay.state === 'on' || relay.state === 'off')
      );

    if (!validRelayPayload) {
      return res.status(400).json({ error: 'Invalid relay state payload' });
    }

    const nextRelayState = state.reportedRelayState.map((relay) => ({ ...relay }));
    relays.forEach((incomingRelay) => {
      const relay = nextRelayState[incomingRelay.channel - 1];
      relay.state = incomingRelay.state;
    });
    state.reportedRelayState = nextRelayState;

    return res.json({ relays: state.reportedRelayState });
  });

  app.post('/api/microcontroller/schedules', requireApiToken, (req, res) => {
    const { schedules } = req.body;
    const validSchedulesPayload =
      Array.isArray(schedules) &&
      schedules.every(
        (schedule) =>
          schedule &&
          Number.isInteger(schedule.channel) &&
          schedule.channel >= 1 &&
          schedule.channel <= RELAY_CHANNELS &&
          typeof schedule.zone === 'string' &&
          schedule.zone.trim().length > 0 &&
          typeof schedule.startTime === 'string' &&
          schedule.startTime.trim().length > 0 &&
          Number.isInteger(schedule.durationSeconds) &&
          schedule.durationSeconds > 0
      );

    if (!validSchedulesPayload) {
      return res.status(400).json({ error: 'Invalid schedule payload' });
    }

    state.schedules = schedules.map((schedule) => ({ ...schedule }));
    return res.json({ schedules: state.schedules });
  });

  app.post('/api/schedules', requireApiToken, (req, res) => {
    const { schedules } = req.body;
    const validSchedulesPayload =
      Array.isArray(schedules) &&
      schedules.length > 0 &&
      schedules.every(
        (schedule) =>
          schedule &&
          Number.isInteger(schedule.channel) &&
          schedule.channel >= 1 &&
          schedule.channel <= RELAY_CHANNELS &&
          typeof schedule.zone === 'string' &&
          schedule.zone.trim().length > 0 &&
          typeof schedule.startTime === 'string' &&
          schedule.startTime.trim().length > 0 &&
          Number.isInteger(schedule.durationSeconds) &&
          schedule.durationSeconds > 0
      );

    if (!validSchedulesPayload) {
      return res.status(400).json({ error: 'Invalid schedule payload' });
    }

    const normalizedSchedules = schedules.map((schedule) => ({
      channel: schedule.channel,
      zone: schedule.zone.trim(),
      startTime: schedule.startTime.trim(),
      durationSeconds: schedule.durationSeconds
    }));

    state.schedules = normalizedSchedules;
    const command = {
      id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      type: 'schedule_update',
      schedules: normalizedSchedules,
      requestedBy: req.body.requestedBy || 'api',
      createdAt: new Date().toISOString(),
      status: 'queued'
    };
    state.queue.push(command);
    wakePendingPolls();
    return res.status(201).json({ schedules: state.schedules, command });
  });

  app.post('/api/commands', requireApiToken, (req, res) => {
    const { channel, action, requestedBy } = req.body;
    const validAction = action === 'on' || action === 'off' || action === 'toggle';

    if (!Number.isInteger(channel) || channel < 1 || channel > RELAY_CHANNELS || !validAction) {
      return res.status(400).json({ error: 'Invalid command payload' });
    }

    const command = createCommand({ channel, action, requestedBy });

    const desiredRelay = state.desiredRelayState[channel - 1];
    desiredRelay.state = action === 'toggle' ? (desiredRelay.state === 'on' ? 'off' : 'on') : action;
    state.queue.push(command);
    wakePendingPolls();
    res.status(201).json({ command });
  });

  app.get('/api/queue/next', requireApiToken, (req, res) => {
    const command = getNextQueuedCommand();
    if (!command) {
      return holdLongPoll(req, res);
    }
    return res.json(deliverCommand(command));
  });


  app.post('/api/microcontroller/state', requireApiToken, (req, res) => {
    const { deviceId, firmwareVersion, clockValid, relays, schedules, currentRun, lastCommandId } = req.body;
    if (!deviceId || !firmwareVersion || !Array.isArray(relays) || !Array.isArray(schedules)) {
      return res.status(400).json({ error: 'Invalid state payload' });
    }

    const validRelayPayload = relays.every((relay) => relay && Number.isInteger(relay.channel) && relay.channel >= 1 && relay.channel <= RELAY_CHANNELS && (relay.state === 'on' || relay.state === 'off'));
    if (!validRelayPayload) {
      return res.status(400).json({ error: 'Invalid state payload' });
    }

    state.reportedRelayState = state.reportedRelayState.map((relay) => ({ ...relay }));
    relays.forEach((incomingRelay) => {
      state.reportedRelayState[incomingRelay.channel - 1].state = incomingRelay.state;
    });
    state.schedules = schedules.map((schedule) => ({ ...schedule }));
    state.deviceTelemetry = { deviceId, firmwareVersion, clockValid: Boolean(clockValid), relays: state.reportedRelayState, schedules: state.schedules, currentRun: currentRun || null, lastCommandId: lastCommandId || null, lastSeenAt: new Date().toISOString() };
    return res.json({ telemetry: state.deviceTelemetry });
  });

  app.post('/api/microcontroller/commands/:id/ack', requireApiToken, (req, res) => {
    const commandIndex = state.queue.findIndex((item) => item.id === req.params.id);
    if (commandIndex < 0) {
      return res.status(404).json({ error: 'Command not found' });
    }
    const command = state.queue[commandIndex];

    const status = req.body.status === 'failed' ? 'failed' : 'applied';
    if (command.status !== 'delivered') {
      return res.status(409).json({ error: 'Command must be delivered before acknowledgement' });
    }

    command.status = status;
    command.acknowledgedAt = new Date().toISOString();
    state.commandHistory.unshift({ ...command });
    state.commandHistory = state.commandHistory.slice(0, 100);
    state.queue.splice(commandIndex, 1);
    return res.json({ command });
  });
  app.get('/api/news', requireApiToken, (_req, res) => {
    res.json({ news: state.news });
  });

  app.post('/api/news', requireApiToken, (req, res) => {
    const { title, body } = req.body;
    if (!title || !body) {
      return res.status(400).json({ error: 'title and body are required' });
    }

    const item = {
      id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      title,
      body,
      createdAt: new Date().toISOString()
    };

    state.news.unshift(item);
    return res.status(201).json({ item });
  });

  app.get('/gui', requireGuiAuth, (_req, res) => {
    const activeZoneIds = state.reportedRelayState
      .filter((relay) => relay.state === 'on')
      .map((relay) => `zone-${relay.channel}`);

    const relayMarkup = state.desiredRelayState
      .map((desiredRelay) => {
        const reportedRelay = state.reportedRelayState[desiredRelay.channel - 1] || { state: 'unknown' };
        const hasMismatch = desiredRelay.state !== reportedRelay.state;
        const statusClass = hasMismatch ? 'status-mismatch' : 'status-synced';

        return `<li class="relay-card">
            <div class="relay-header">
              <span class="relay-title">Zone ${desiredRelay.channel}</span>
              <span class="status-pill ${statusClass}">${hasMismatch ? 'MISMATCH' : 'SYNCED'}</span>
            </div>
            <div class="relay-states">
              <span>Desired <strong>${desiredRelay.state.toUpperCase()}</strong></span>
              <span>Reported <strong>${reportedRelay.state.toUpperCase()}</strong></span>
            </div>
            <div class="relay-actions">
              <form method="post" action="/gui/relays/${desiredRelay.channel}/on">
                <button type="submit" ${reportedRelay.state === 'on' ? 'disabled' : ''}>Turn ON</button>
              </form>
              <form method="post" action="/gui/relays/${desiredRelay.channel}/off">
                <button type="submit" ${reportedRelay.state === 'off' ? 'disabled' : ''}>Turn OFF</button>
              </form>
            </div>
          </li>`;
      })
      .join('');

    const schedulesMarkup = state.schedules.length
      ? `<ul>${state.schedules.map((schedule) => `<li>${formatScheduleLabel(schedule)}</li>`).join('')}</ul>`
      : '<p>No schedules configured.</p>';

    const defaultSchedule = state.schedules.find((schedule) => typeof schedule === 'object') || {};

    res.type('html').send(`<!doctype html>
<html>
  <head>
    <title>Garden Controller</title>
    <meta http-equiv="refresh" content="1" />
    <style>
      :root { color-scheme: dark; --glow: 0 0 18px rgba(120,255,221,0.55); }
      body { font-family: Inter, system-ui, sans-serif; margin: 0; color: #d8e6ff; background: radial-gradient(circle at top, #17331f 0%, #081628 38%, #03060f 100%); }
      .shell { max-width: 1200px; margin: 0 auto; padding: 24px; }
      .panel { background: rgba(8, 20, 30, 0.74); border: 1px solid rgba(128, 255, 199, 0.35); border-radius: 18px; box-shadow: 0 0 30px rgba(98, 255, 210, 0.2); backdrop-filter: blur(8px); }
      h1,h2,h3 { color: #8fe8ff; letter-spacing: 0.03em; }
      .hero { padding: 20px; margin-bottom: 18px; }
      .timestamp { color: #93b9ff; }
      .layout { display: grid; grid-template-columns: 1.1fr 1fr; gap: 18px; }
      .map-wrap { padding: 16px; }
      .map-wrap svg { width: 100%; height: auto; background: linear-gradient(160deg, #020617, #071a32); border-radius: 14px; }
      .zone { fill: rgba(111, 255, 205, 0.09); stroke: #72ffcb; stroke-width: 2.2; transition: fill 120ms linear, filter 120ms linear, stroke 120ms linear; }
      .zone-active { fill: rgba(104, 255, 140, 0.45); stroke: #c0ffd2; filter: drop-shadow(var(--glow)); }
      .linework { stroke: rgba(132, 163, 255, 0.6); }
      .relay-section { padding: 16px; }
      .relay-grid { list-style: none; padding: 0; margin: 0; display: grid; grid-template-columns: repeat(2, minmax(0,1fr)); gap: 12px; }
      .relay-card { border: 1px solid rgba(127, 188, 255, 0.35); border-radius: 14px; padding: 12px; background: rgba(7, 17, 42, 0.85); }
      .relay-header, .relay-states, .relay-actions { display: flex; justify-content: space-between; gap: 10px; align-items: center; }
      .relay-states { font-size: 0.88rem; color: #b6d2ff; margin: 8px 0 12px; }
      .status-pill { font-size: 0.7rem; padding: 4px 8px; border-radius: 999px; font-weight: 700; }
      .status-mismatch { background: rgba(255, 88, 132, 0.22); color: #ff8fb0; }
      .status-synced { background: rgba(83, 255, 204, 0.18); color: #8bffd3; }
      button { border: 1px solid rgba(113, 255, 250, 0.5); color: #bcf9ff; background: linear-gradient(180deg, #103a61, #0b203f); padding: 7px 11px; border-radius: 10px; cursor: pointer; }
      button:disabled { opacity: 0.4; cursor: not-allowed; }
      .schedule { margin-top: 18px; padding: 16px; }
      .schedule form { display: grid; grid-template-columns: repeat(2, minmax(0,1fr)); gap: 10px; }
      label { display: flex; flex-direction: column; font-size: 0.85rem; color: #a9c4ff; }
      input { margin-top: 6px; border-radius: 10px; border: 1px solid rgba(128, 173, 255, 0.35); background: #060d20; color: #d8e6ff; padding: 8px; }
      @media (max-width: 900px) { .layout { grid-template-columns: 1fr; } .relay-grid, .schedule form { grid-template-columns: 1fr; } }
    </style>
  </head>
  <body>
    <div class="shell">
      <section class="panel hero">
        <h1>ESP32-S3-Relay-6CH Controller · Garden Fresh Futurism</h1>
        <p class="timestamp">Current server time (UTC): <strong>${new Date().toISOString()}</strong></p>
        <p class="timestamp">Auto-refresh: every 1 second · Active zones glowing on map.</p>
      </section>
      <div class="layout">
        <section class="panel map-wrap">
          <h2>Garden Zone Map</h2>
          <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 295.743 295.482" role="img" aria-label="Garden watering zones">
            <g class="linework"><polyline points="10,266.311 251.526,274.667"/><line x1="285.743" y1="285.482" x2="282.082" y2="191.277"/><polyline points="282.082,191.277 276.705,168.862 277.246,95.213 277.7,15.387"/><polyline points="13.295,258.387 35.211,10 37.872,11.914 276.96,14.582"/></g>
            <polygon id="zone-1" class="zone ${activeZoneIds.includes('zone-1') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-1')}" points="127.534,159.189 124.128,239.478 15.618,232.055 22.68,152.017 127.534,159.189"/>
            <polygon id="zone-2" class="zone ${activeZoneIds.includes('zone-2') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-2')}" points="146.876,95.99 134.762,166.15 198.935,169.661 205.581,96.842 146.876,95.99"/>
            <polygon id="zone-3" class="zone ${activeZoneIds.includes('zone-3') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-3')}" points="205.581,96.842 264.287,97.694 263.108,173.173 198.935,169.661 205.581,96.842"/>
            <polygon id="zone-4" class="zone ${activeZoneIds.includes('zone-4') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-4')}" points="32,46.388 46.493,52.635 95.676,139.247 128.342,140.128 127.534,159.189 22.68,152.017 32,46.388"/>
            <polygon id="zone-5" class="zone ${activeZoneIds.includes('zone-5') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-5')}" points="152.903,89.893 152.691,13.195 50.949,12.06 136.836,89.869 152.903,89.893"/>
            <polygon id="zone-6" class="zone ${activeZoneIds.includes('zone-6') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-6')}" points="249.304,90.041 249.095,14.271 152.691,13.195 152.903,89.893 249.304,90.041"/>
          </svg>
        </section>
        <section class="panel relay-section">
          <h2>Relay states (desired vs reported)</h2>
          <ul class="relay-grid">${relayMarkup}</ul>
        </section>
      </div>
      <section class="panel schedule">
        <h2>Schedules</h2>
        ${schedulesMarkup}
        <h3>Update schedules</h3>
        <form method="post" action="/gui/schedules">
          <label>Zone <input name="zone" value="${defaultSchedule.zone || ''}" required /></label>
          <label>Channel <input name="channel" type="number" min="1" max="${RELAY_CHANNELS}" value="${defaultSchedule.channel || 1}" required /></label>
          <label>Start Time <input name="startTime" value="${defaultSchedule.startTime || '06:00'}" required /></label>
          <label>Duration (seconds) <input name="durationSeconds" type="number" min="1" value="${defaultSchedule.durationSeconds || 900}" required /></label>
          <button type="submit">Save schedule</button>
        </form>
      </section>
    </div>
  </body>
</html>`);
;
  });

  app.post('/gui/relays/:channel/:action', requireGuiAuth, (req, res) => {
    const channel = Number.parseInt(req.params.channel, 10);
    const action = req.params.action;

    if (!Number.isInteger(channel) || channel < 1 || channel > RELAY_CHANNELS) {
      return res.status(400).send('Invalid relay channel');
    }

    if (action !== 'on' && action !== 'off') {
      return res.status(400).send('Invalid relay action');
    }

    const command = createCommand({ channel, action, requestedBy: 'gui-web' });
    const desiredRelay = state.desiredRelayState[channel - 1];
    desiredRelay.state = action;
    state.queue.push(command);
    wakePendingPolls();
    return res.redirect(303, '/gui');
  });

  app.post('/gui/schedules', requireGuiAuth, (req, res) => {
    const channel = Number.parseInt(req.body.channel, 10);
    const durationSeconds = Number.parseInt(req.body.durationSeconds, 10);
    const zone = typeof req.body.zone === 'string' ? req.body.zone.trim() : '';
    const startTime = typeof req.body.startTime === 'string' ? req.body.startTime.trim() : '';

    if (
      !Number.isInteger(channel) ||
      channel < 1 ||
      channel > RELAY_CHANNELS ||
      !Number.isInteger(durationSeconds) ||
      durationSeconds <= 0 ||
      !zone ||
      !startTime
    ) {
      return res.status(400).send('Invalid schedule payload');
    }

    const schedule = { channel, zone, startTime, durationSeconds };
    state.schedules = [schedule];
    const command = {
      id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      type: 'schedule_update',
      schedules: [schedule],
      requestedBy: 'gui-web',
      createdAt: new Date().toISOString(),
      status: 'queued'
    };
    state.queue.push(command);
    wakePendingPolls();
    return res.redirect(303, '/gui');
  });

  app.get('/openapi.json', (_req, res) => {
    const specPath = path.join(__dirname, '..', 'openapi.yaml');
    const raw = fs.readFileSync(specPath, 'utf8');
    const spec = yaml.load(raw);
    res.json(spec);
  });

  return { app, state };
}

module.exports = { createApp, createState, RELAY_CHANNELS };
