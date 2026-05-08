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
    relayState: Array.from({ length: RELAY_CHANNELS }, (_, index) => ({ channel: index + 1, state: 'off' })),
    queue: [],
    news: [],
    schedules: []
  };
}

function createCommand({ channel, action, requestedBy }) {
  return {
    id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
    channel,
    action,
    requestedBy: requestedBy || 'gui',
    createdAt: new Date().toISOString()
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
  const guiUsername = normalizeCredentialValue(config.guiUsername ?? process.env.GUI_USERNAME);
  const guiPassword = normalizeCredentialValue(config.guiPassword ?? process.env.GUI_PASSWORD);
  const apiToken = normalizeCredentialValue(config.apiToken ?? process.env.API_TOKEN);

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
    res.json({ relays: state.relayState });
  });

  app.get('/api/state', requireApiToken, (_req, res) => {
    res.json({
      relays: state.relayState,
      queueDepth: state.queue.length,
      serverTime: new Date().toISOString(),
      schedules: state.schedules
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

    const nextRelayState = state.relayState.map((relay) => ({ ...relay }));
    relays.forEach((incomingRelay) => {
      const relay = nextRelayState[incomingRelay.channel - 1];
      relay.state = incomingRelay.state;
    });
    state.relayState = nextRelayState;

    return res.json({ relays: state.relayState });
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
      createdAt: new Date().toISOString()
    };
    state.queue.push(command);
    return res.status(201).json({ schedules: state.schedules, command });
  });

  app.post('/api/commands', requireApiToken, (req, res) => {
    const { channel, action, requestedBy } = req.body;
    const validAction = action === 'on' || action === 'off' || action === 'toggle';

    if (!Number.isInteger(channel) || channel < 1 || channel > RELAY_CHANNELS || !validAction) {
      return res.status(400).json({ error: 'Invalid command payload' });
    }

    const command = createCommand({ channel, action, requestedBy });

    state.queue.push(command);
    res.status(201).json({ command });
  });

  app.get('/api/queue/next', requireApiToken, (_req, res) => {
    const command = state.queue.shift();
    if (!command) {
      return res.status(204).send();
    }

    if (command.type === 'schedule_update') {
      return res.json({ command });
    }

    const relay = state.relayState[command.channel - 1];
    if (command.action === 'toggle') {
      relay.state = relay.state === 'on' ? 'off' : 'on';
    } else {
      relay.state = command.action;
    }

    return res.json({ command, relay: { ...relay } });
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
    const relayMarkup = state.relayState
      .map(
        (relay) => `<li>
            Channel ${relay.channel}: <strong>${relay.state.toUpperCase()}</strong>
            <form method="post" action="/gui/relays/${relay.channel}/toggle" style="display:inline; margin-left:8px;">
              <button type="submit">Toggle</button>
            </form>
          </li>`
      )
      .join('');

    const schedulesMarkup = state.schedules.length
      ? `<ul>${state.schedules.map((schedule) => `<li>${formatScheduleLabel(schedule)}</li>`).join('')}</ul>`
      : '<p>No schedules configured.</p>';

    const defaultSchedule = state.schedules.find((schedule) => typeof schedule === 'object') || {};

    res.type('html').send(`<!doctype html>
<html>
  <head><title>Garden Controller</title></head>
  <body>
    <h1>ESP32-S3-Relay-6CH Controller</h1>
    <p>Current server time (UTC): <strong>${new Date().toISOString()}</strong></p>
    <h2>Relay state</h2>
    <ul>${relayMarkup}</ul>
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
  </body>
</html>`);
  });

  app.post('/gui/relays/:channel/toggle', requireGuiAuth, (req, res) => {
    const channel = Number.parseInt(req.params.channel, 10);
    if (!Number.isInteger(channel) || channel < 1 || channel > RELAY_CHANNELS) {
      return res.status(400).send('Invalid relay channel');
    }

    state.queue.push(createCommand({ channel, action: 'toggle', requestedBy: 'gui-web' }));
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
    state.queue.push({
      id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      type: 'schedule_update',
      schedules: [schedule],
      requestedBy: 'gui-web',
      createdAt: new Date().toISOString()
    });
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
