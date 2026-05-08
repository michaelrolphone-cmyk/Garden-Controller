const express = require('express');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');

const RELAY_CHANNELS = 6;

function normalizeCredentialValue(value) {
  return typeof value === 'string' ? value.trim() : value;
}

function createState() {
  return {
    relayState: Array.from({ length: RELAY_CHANNELS }, (_, index) => ({ channel: index + 1, state: 'off' })),
    queue: [],
    news: []
  };
}

function createApp(config = {}) {
  const app = express();
  app.use(express.json());

  const state = config.state || createState();
  const guiUsername = normalizeCredentialValue(config.guiUsername ?? process.env.GUI_USERNAME);
  const guiPassword = normalizeCredentialValue(config.guiPassword ?? process.env.GUI_PASSWORD);
  const apiToken = config.apiToken ?? process.env.API_TOKEN;

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

  app.post('/api/commands', requireApiToken, (req, res) => {
    const { channel, action, requestedBy } = req.body;
    const validAction = action === 'on' || action === 'off' || action === 'toggle';

    if (!Number.isInteger(channel) || channel < 1 || channel > RELAY_CHANNELS || !validAction) {
      return res.status(400).json({ error: 'Invalid command payload' });
    }

    const command = {
      id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      channel,
      action,
      requestedBy: requestedBy || 'gui',
      createdAt: new Date().toISOString()
    };

    state.queue.push(command);
    res.status(201).json({ command });
  });

  app.get('/api/queue/next', requireApiToken, (_req, res) => {
    const command = state.queue.shift();
    if (!command) {
      return res.status(204).send();
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
      .map((relay) => `<li>Channel ${relay.channel}: <strong>${relay.state.toUpperCase()}</strong></li>`)
      .join('');

    res.type('html').send(`<!doctype html>
<html>
  <head><title>Garden Controller</title></head>
  <body>
    <h1>ESP32-S3-Relay-6CH Controller</h1>
    <p>Use the API to queue commands for the microcontroller.</p>
    <ul>${relayMarkup}</ul>
  </body>
</html>`);
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
