const express = require('express');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');

const ZONE_CHANNELS = 5;
const RELAY_CHANNELS = 6;
const MASTER_VALVE_CHANNEL = 6;
const DEFAULT_ON_DURATION_MINUTES = 15;
const DEFAULT_ON_DURATION_SECONDS = DEFAULT_ON_DURATION_MINUTES * 60;


const GARDEN_LOCATION = { lat: 43.665288, lon: -116.259186, label: 'garden' };

function buildEnvironmentalFeeds() {
  const { lat: latitude, lon: longitude } = GARDEN_LOCATION;
  return {
    radarEmbedUrl: `https://embed.windy.com/embed2.html?lat=${latitude}&lon=${longitude}&zoom=8&level=surface&overlay=radar&product=radar`,
    satelliteEmbedUrl: `https://embed.windy.com/embed2.html?lat=${latitude}&lon=${longitude}&zoom=6&level=surface&overlay=satellite&product=ecmwf`,
    lidarMapUrl: `https://apps.nationalmap.gov/viewer/?basemap=3&ll=${longitude},${latitude}&zoom=13`,
    nsslRadarUrl: `https://mrms.nssl.noaa.gov/qvs/product_viewer/?lat=${latitude}&lon=${longitude}&prod=radaronly`,
    noaaSatelliteUrl: `https://www.star.nesdis.noaa.gov/GOES/sector_band.php?sat=G17&sector=pnw&band=GEOCOLOR&length=24`
  };
}

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
    desiredRelayState: Array.from({ length: RELAY_CHANNELS }, (_, index) => ({
      channel: index + 1,
      state: 'off',
      role: index + 1 === MASTER_VALVE_CHANNEL ? 'master_valve_spigots' : 'zone',
      zone: index + 1 <= ZONE_CHANNELS ? index + 1 : null
    })),
    reportedRelayState: Array.from({ length: RELAY_CHANNELS }, (_, index) => ({
      channel: index + 1,
      state: 'off',
      role: index + 1 === MASTER_VALVE_CHANNEL ? 'master_valve_spigots' : 'zone',
      zone: index + 1 <= ZONE_CHANNELS ? index + 1 : null
    })),
    queue: [],
    commandHistory: [],
    news: [],
    schedules: [],
    deviceTelemetry: null,
    sensorReadings: [],
    latestSensorData: null,
    spigotRun: {
      active: false,
      remainingSeconds: 0,
      updatedAt: null
    },
    weatherDatasets: {
      current: null,
      forecast: null,
      radar: null,
      satellite: null,
      updatedAt: null
    }
  };
}


function renderSensorDataMarkup(latestSensorData, deviceTelemetry) {
  const telemetrySensorData = deviceTelemetry && Array.isArray(deviceTelemetry.sensorData) ? deviceTelemetry.sensorData : [];
  const readings = latestSensorData && Array.isArray(latestSensorData.sensorData)
    ? latestSensorData.sensorData
    : telemetrySensorData;

  if (!readings.length) {
    return '<p id="sensor-empty">No microcontroller weather sensor data reported yet.</p>';
  }

  const rows = readings
    .map((reading) => `<tr><td>${reading.type || 'unknown'}</td><td>${reading.value ?? 'n/a'}</td><td>${reading.unit || ''}</td><td>${reading.source || 'firmware'}</td></tr>`)
    .join('');

  return `<table class="sensor-table" id="sensor-table"><thead><tr><th>Metric</th><th>Value</th><th>Unit</th><th>Source</th></tr></thead><tbody>${rows}</tbody></table>`;
}

function formatTimeSince(lastSeenAt, now = Date.now()) {
  if (!lastSeenAt) return 'never';
  const timestamp = Date.parse(lastSeenAt);
  if (Number.isNaN(timestamp)) return 'unknown';
  const diffSeconds = Math.max(0, Math.floor((now - timestamp) / 1000));
  if (diffSeconds < 60) return `${diffSeconds}s ago`;
  if (diffSeconds < 3600) return `${Math.floor(diffSeconds / 60)}m ago`;
  if (diffSeconds < 86400) return `${Math.floor(diffSeconds / 3600)}h ago`;
  return `${Math.floor(diffSeconds / 86400)}d ago`;
}

function renderTelemetryMarkup(deviceTelemetry) {
  if (!deviceTelemetry) {
    return '<p id="telemetry-empty">No device telemetry reported yet.</p>';
  }

  const telemetryPairs = [
    ['Device ID', deviceTelemetry.deviceId || 'n/a'],
    ['Firmware', deviceTelemetry.firmwareVersion || 'n/a'],
    ['Clock Valid', String(Boolean(deviceTelemetry.clockValid))],
    ['Last Command ID', deviceTelemetry.lastCommandId || 'n/a'],
    ['Current Run', deviceTelemetry.currentRun ? JSON.stringify(deviceTelemetry.currentRun) : 'none'],
    ['Target Location', deviceTelemetry.targetLocation ? JSON.stringify(deviceTelemetry.targetLocation) : 'n/a'],
    ['Last Seen (UTC)', deviceTelemetry.lastSeenAt || 'n/a'],
    ['Last Seen', formatTimeSince(deviceTelemetry.lastSeenAt)]
  ];

  const rows = telemetryPairs.map(([label, value]) => `<tr><th>${label}</th><td>${value}</td></tr>`).join('');
  return `<table class="telemetry-table" id="telemetry-table"><tbody>${rows}</tbody></table>`;
}

function createRelayCommand({ channel, action, requestedBy }) {
  return {
    id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
    channel,
    action,
    requestedBy: requestedBy || 'gui',
    createdAt: new Date().toISOString(),
    status: 'queued'
  };
}


function zoneShapeIdForChannel(channel) {
  const mapping = {
    1: 'zone-3',
    2: 'zone-6',
    3: 'zone-5',
    4: 'zone-1 zone-2',
    5: 'zone-4'
  };
  return mapping[channel] || `zone-${channel}`;
}

function zoneShapeIdsForChannel(channel) {
  return String(zoneShapeIdForChannel(channel)).split(' ');
}

function createSpigotCommand({ action, durationSeconds, requestedBy }) {
  return {
    id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
    channel: MASTER_VALVE_CHANNEL,
    action,
    durationSeconds: Number.isInteger(durationSeconds) && durationSeconds > 0 ? durationSeconds : DEFAULT_ON_DURATION_SECONDS,
    requestedBy: requestedBy || 'gui',
    createdAt: new Date().toISOString(),
    status: 'queued',
    role: 'master_valve_spigots'
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



  async function refreshWeatherDatasets() {
    const pointUrl = `https://api.weather.gov/points/${GARDEN_LOCATION.lat},${GARDEN_LOCATION.lon}`;

    const headers = {
      'User-Agent': 'Garden-Controller/1.0 contact@example.com',
      Accept: 'application/geo+json'
    };

    const pointRes = await fetch(pointUrl, { headers });
    if (!pointRes.ok) {
      throw new Error(`NWS point lookup failed: ${pointRes.status}`);
    }

    const pointData = await pointRes.json();
    const props = pointData.properties || {};

    const [forecastRes, hourlyRes, gridRes] = await Promise.all([
      props.forecast ? fetch(props.forecast, { headers }) : null,
      props.forecastHourly ? fetch(props.forecastHourly, { headers }) : null,
      props.forecastGridData ? fetch(props.forecastGridData, { headers }) : null
    ]);

    const forecast = forecastRes && forecastRes.ok ? await forecastRes.json() : null;
    const hourly = hourlyRes && hourlyRes.ok ? await hourlyRes.json() : null;
    const grid = gridRes && gridRes.ok ? await gridRes.json() : null;

    state.weatherDatasets.current = { source: 'NWS', point: props, grid, updatedAt: new Date().toISOString() };
    state.weatherDatasets.forecast = { source: 'NWS', forecast, hourly, updatedAt: new Date().toISOString() };
    state.weatherDatasets.radar = {
      source: 'NOAA/NEXRAD',
      targetLocation: GARDEN_LOCATION,
      status: 'metadata-placeholder',
      note: 'Add NEXRAD/MRMS tile or image service integration here.',
      updatedAt: new Date().toISOString()
    };
    state.weatherDatasets.satellite = {
      source: 'NOAA/GOES',
      targetLocation: GARDEN_LOCATION,
      status: 'metadata-placeholder',
      note: 'Add GOES/GEOSS tile or image service integration here.',
      updatedAt: new Date().toISOString()
    };
    state.weatherDatasets.updatedAt = new Date().toISOString();

    return state.weatherDatasets;
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
      zoneChannels: ZONE_CHANNELS,
      relayChannels: RELAY_CHANNELS,
      masterValveChannel: MASTER_VALVE_CHANNEL,
      desiredRelays: state.desiredRelayState,
      reportedRelays: state.reportedRelayState,
      masterValveState: state.reportedRelayState[MASTER_VALVE_CHANNEL - 1],
      spigotRun: state.spigotRun,
      queueDepth: state.queue.filter((command) => command.status === 'queued').length,
      deliveredDepth: state.queue.filter((command) => command.status === 'delivered').length,
      pendingPolls: pendingPolls.length,
      commandHistory: state.commandHistory,
      serverTime: new Date().toISOString(),
      latestSensorData: state.latestSensorData,
      schedules: state.schedules,
      deviceTelemetry: state.deviceTelemetry,
      sensorReadingCount: state.sensorReadings.length,
      weatherDatasets: state.weatherDatasets
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
      relay.role = incomingRelay.role || (incomingRelay.channel === MASTER_VALVE_CHANNEL ? 'master_valve_spigots' : 'zone');
      relay.zone = incomingRelay.channel <= ZONE_CHANNELS ? incomingRelay.channel : null;
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
          schedule.channel <= ZONE_CHANNELS &&
          typeof schedule.zone === 'string' &&
          schedule.zone.trim().length > 0 &&
          typeof schedule.startTime === 'string' &&
          schedule.startTime.trim().length > 0 &&
          Number.isInteger(schedule.durationSeconds) &&
          schedule.durationSeconds > 0
      );

    if (!validSchedulesPayload) {
      return res.status(400).json({
        error: 'Invalid schedule payload',
        detail: 'Schedules may only use zones/channels 1-5. Channel 6 is master valve/spigots.'
      });
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
          schedule.channel <= ZONE_CHANNELS &&
          typeof schedule.zone === 'string' &&
          schedule.zone.trim().length > 0 &&
          typeof schedule.startTime === 'string' &&
          schedule.startTime.trim().length > 0 &&
          Number.isInteger(schedule.durationSeconds) &&
          schedule.durationSeconds > 0
      );

    if (!validSchedulesPayload) {
      return res.status(400).json({
        error: 'Invalid schedule payload',
        detail: 'Schedules may only use zones/channels 1-5. Channel 6 is master valve/spigots.'
      });
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
    const durationSeconds = Number.parseInt(req.body.durationSeconds, 10);
    const validAction = action === 'on' || action === 'off' || action === 'toggle';

    if (!Number.isInteger(channel) || channel < 1 || channel > RELAY_CHANNELS || !validAction) {
      return res.status(400).json({ error: 'Invalid command payload' });
    }

    let command;
    if (channel === MASTER_VALVE_CHANNEL) {
      command = createSpigotCommand({ action, durationSeconds, requestedBy: requestedBy || 'api' });
      const desiredMaster = state.desiredRelayState[MASTER_VALVE_CHANNEL - 1];
      if (action === 'off') {
        desiredMaster.state = 'off';
        state.spigotRun = { active: false, remainingSeconds: 0, updatedAt: new Date().toISOString() };
      } else {
        desiredMaster.state = 'on';
        state.spigotRun = { active: true, remainingSeconds: command.durationSeconds, updatedAt: new Date().toISOString() };
      }
    } else {
      command = createRelayCommand({ channel, action, requestedBy: requestedBy || 'api' });
      if (action === 'on') {
        command.durationSeconds = Number.isInteger(durationSeconds) && durationSeconds > 0 ? durationSeconds : DEFAULT_ON_DURATION_SECONDS;
      }
      const desiredRelay = state.desiredRelayState[channel - 1];
      desiredRelay.state = action === 'toggle' ? (desiredRelay.state === 'on' ? 'off' : 'on') : action;
    }

    state.queue.push(command);
    wakePendingPolls();
    res.status(201).json({ command });
  });

  app.post('/api/spigots/run', requireApiToken, (req, res) => {
    const durationSeconds = Number.parseInt(req.body.durationSeconds, 10);
    const command = createSpigotCommand({ action: 'on', durationSeconds, requestedBy: req.body.requestedBy || 'api' });
    state.desiredRelayState[MASTER_VALVE_CHANNEL - 1].state = 'on';
    state.spigotRun = { active: true, remainingSeconds: command.durationSeconds, updatedAt: new Date().toISOString() };
    state.queue.push(command);
    wakePendingPolls();
    return res.status(201).json({ command, spigotRun: state.spigotRun });
  });

  app.post('/api/spigots/stop', requireApiToken, (req, res) => {
    const command = createSpigotCommand({ action: 'off', durationSeconds: 0, requestedBy: req.body.requestedBy || 'api' });
    state.desiredRelayState[MASTER_VALVE_CHANNEL - 1].state = 'off';
    state.spigotRun = { active: false, remainingSeconds: 0, updatedAt: new Date().toISOString() };
    state.queue.push(command);
    wakePendingPolls();
    return res.status(201).json({ command, spigotRun: state.spigotRun });
  });

  app.get('/api/queue/next', requireApiToken, (req, res) => {
    const command = getNextQueuedCommand();
    if (!command) {
      return holdLongPoll(req, res);
    }
    return res.json(deliverCommand(command));
  });



  app.post('/api/microcontroller/sensors', requireApiToken, (req, res) => {
    const { deviceId, firmwareVersion, targetLocation, sensorData } = req.body;

    const validLocation =
      targetLocation &&
      Number.isFinite(Number(targetLocation.lat)) &&
      Number.isFinite(Number(targetLocation.lon));

    const validSensorData =
      Array.isArray(sensorData) &&
      sensorData.every((item) => item && typeof item.source === 'string' && typeof item.type === 'string');

    if (!deviceId || !firmwareVersion || !validLocation || !validSensorData) {
      return res.status(400).json({ error: 'Invalid sensor payload' });
    }

    const reading = {
      id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      deviceId,
      firmwareVersion,
      targetLocation: { lat: Number(targetLocation.lat), lon: Number(targetLocation.lon), label: targetLocation.label || 'garden' },
      sensorData: sensorData.map((item) => ({ ...item })),
      receivedAt: new Date().toISOString()
    };

    state.latestSensorData = reading;
    state.sensorReadings.unshift(reading);
    state.sensorReadings = state.sensorReadings.slice(0, 1000);

    return res.status(201).json({ reading });
  });

  app.post('/api/microcontroller/state', requireApiToken, (req, res) => {
    const {
      deviceId,
      firmwareVersion,
      clockValid,
      epoch,
      localTime,
      localDate,
      homeWifiConnected,
      homeIp,
      relays,
      schedules,
      currentRun,
      spigotRun,
      masterValveChannel,
      masterValveOn,
      lastCommandId,
      targetLocation,
      sensorData
    } = req.body;
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
    state.spigotRun = spigotRun || state.spigotRun;
    state.deviceTelemetry = {
      deviceId,
      firmwareVersion,
      clockValid: Boolean(clockValid),
      epoch,
      localTime,
      localDate,
      homeWifiConnected,
      homeIp,
      relays: state.reportedRelayState,
      schedules: state.schedules,
      currentRun: currentRun || null,
      masterValveChannel: masterValveChannel || MASTER_VALVE_CHANNEL,
      masterValveOn: Boolean(masterValveOn),
      spigotRun: spigotRun || null,
      lastCommandId: lastCommandId || null,
      targetLocation: targetLocation || null,
      sensorData: Array.isArray(sensorData) ? sensorData : [],
      lastSeenAt: new Date().toISOString()
    };

    if (targetLocation && Array.isArray(sensorData)) {
      const reading = {
        id: `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
        deviceId,
        firmwareVersion,
        targetLocation,
        sensorData,
        receivedAt: new Date().toISOString(),
        sourceEndpoint: '/api/microcontroller/state'
      };

      state.latestSensorData = reading;
      state.sensorReadings.unshift(reading);
      state.sensorReadings = state.sensorReadings.slice(0, 1000);
    }
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


  app.get('/api/sensors', requireApiToken, (req, res) => {
    const limit = Math.max(1, Math.min(Number.parseInt(req.query.limit, 10) || 100, 1000));
    res.json({ readings: state.sensorReadings.slice(0, limit), latest: state.latestSensorData });
  });

  app.get('/api/sensors/latest', requireApiToken, (_req, res) => {
    res.json({ latest: state.latestSensorData });
  });

  app.get('/api/weather/datasets', requireApiToken, (_req, res) => {
    res.json({ targetLocation: GARDEN_LOCATION, datasets: state.weatherDatasets });
  });

  app.get('/api/weather/current', requireApiToken, (_req, res) => {
    res.json({ targetLocation: GARDEN_LOCATION, current: state.weatherDatasets.current });
  });

  app.get('/api/weather/forecast', requireApiToken, (_req, res) => {
    res.json({ targetLocation: GARDEN_LOCATION, forecast: state.weatherDatasets.forecast });
  });

  app.get('/api/weather/radar', requireApiToken, (_req, res) => {
    res.json({ targetLocation: GARDEN_LOCATION, radar: state.weatherDatasets.radar });
  });

  app.get('/api/weather/satellite', requireApiToken, (_req, res) => {
    res.json({ targetLocation: GARDEN_LOCATION, satellite: state.weatherDatasets.satellite });
  });

  app.post('/api/weather/refresh', requireApiToken, async (_req, res) => {
    try {
      const datasets = await refreshWeatherDatasets();
      res.json({ datasets });
    } catch (error) {
      res.status(502).json({ error: 'Weather refresh failed', message: error.message });
    }
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
      .map((relay) => zoneShapeIdForChannel(relay.channel));

    const relayMarkup = state.desiredRelayState
      .filter((relay) => relay.channel <= ZONE_CHANNELS)
      .map((desiredRelay) => {
        const reportedRelay = state.reportedRelayState[desiredRelay.channel - 1] || { state: 'unknown' };
        const hasMismatch = desiredRelay.state !== reportedRelay.state;
        const statusClass = hasMismatch ? 'status-mismatch' : 'status-synced';

        return `<li class="relay-card zone-theme-\${desiredRelay.channel}">
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
                <input name="minutes" type="number" min="1" max="240" value="15" />
                <button type="submit" ${reportedRelay.state === 'on' ? 'disabled' : ''}>Run</button>
              </form>
              <form method="post" action="/gui/relays/${desiredRelay.channel}/off">
                <button type="submit" ${reportedRelay.state === 'off' ? 'disabled' : ''}>Stop</button>
              </form>
            </div>
          </li>`;
      })
      .join('');

    const envFeeds = buildEnvironmentalFeeds();
    const parsedSchedules = state.schedules.filter((schedule) => typeof schedule === 'object' && schedule).map((schedule) => ({ ...schedule }));
    const scheduleTimelineMarkup = parsedSchedules.length
      ? `<div class="timeline">${parsedSchedules
          .map((schedule) => {
            const [hoursText = '0', minutesText = '0'] = String(schedule.startTime || '00:00').split(':');
            const startMinutes = Number.parseInt(hoursText, 10) * 60 + Number.parseInt(minutesText, 10);
            const leftPercent = Math.max(0, Math.min((startMinutes / 1440) * 100, 100));
            const widthPercent = Math.max(2, Math.min(((Number(schedule.durationSeconds) || 60) / 86400) * 100, 100 - leftPercent));
            return `<div class="timeline-row"><span class="timeline-zone">${schedule.zone}</span><div class="timeline-track"><span class="timeline-block" style="left:${leftPercent}%;width:${widthPercent}%">${schedule.startTime} · ${schedule.durationSeconds}s</span></div></div>`;
          })
          .join('')}</div>`
      : '<p>No schedules configured.</p>';
    const schedulesMarkup = state.schedules.length ? `<ul>${state.schedules.map((schedule) => `<li>${formatScheduleLabel(schedule)}</li>`).join('')}</ul>` : '';
    const defaultSchedule = state.schedules.find((schedule) => typeof schedule === 'object') || {};
    const sensorDataMarkup = renderSensorDataMarkup(state.latestSensorData, state.deviceTelemetry);
    const telemetryMarkup = renderTelemetryMarkup(state.deviceTelemetry);

    res.type('html').send(`<!doctype html>
<html>
  <head>
    <title>Garden Controller</title>
    <style>
      :root { color-scheme: dark; --glow: 0 0 18px rgba(120,255,221,0.55); }
      body { font-family: Inter, system-ui, sans-serif; margin: 0; color: #d8e6ff; background: #f1f5f9; }
      .shell { max-width: 1400px; margin: 0 auto; padding: 16px; }
      .panel { background: #ffffff; border: 1px solid #dbe7ef; border-radius: 18px; color:#123; box-shadow: 0 6px 18px rgba(15,23,42,0.07); }
      h1,h2,h3 { color: #0f2d3f; letter-spacing: 0.01em; margin:0; }
      .hero { padding: 20px; margin-bottom: 14px; display:flex; justify-content:space-between; align-items:center; }
      .timestamp { color: #5d7284; margin:4px 0 0; }
      .layout { display: grid; grid-template-columns: 1fr 2fr; gap: 14px; }
      .map-wrap { padding: 16px; } .map-wrap p,.relay-section p{color:#4f6474;}
      .map-wrap svg { width: 100%; height: auto; background: linear-gradient(160deg, #dbeafe, #eff6ff); border-radius: 14px; }
      .zone { fill: rgba(var(--zone-color-rgb, 62, 184, 255), 0.2); stroke: rgba(var(--zone-color-rgb, 62, 184, 255), 0.95); stroke-width: 2; transition: fill .25s ease, stroke .25s ease, filter .25s ease; } .zone-active { fill: rgba(var(--zone-color-rgb, 62, 184, 255), 0.38); stroke: rgba(var(--zone-color-rgb, 62, 184, 255), 1); filter: drop-shadow(0 0 16px rgba(var(--zone-color-rgb, 62, 184, 255), 0.55)); } .zone-theme-1{--zone-color-rgb:102,163,69;} .zone-theme-2{--zone-color-rgb:245,179,52;} .zone-theme-3{--zone-color-rgb:42,178,171;} .zone-theme-4{--zone-color-rgb:129,97,199;} .zone-theme-5{--zone-color-rgb:236,125,60;} .zone-theme-6{--zone-color-rgb:67,137,196;}
      .linework { stroke: rgba(29, 78, 216, 0.35); fill: none; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; } .relay-section { padding: 16px; }
      .relay-grid { list-style: none; padding: 0; margin: 0; display: grid; grid-template-columns: repeat(2, minmax(0,1fr)); gap: 12px; }
      .relay-card { border: 1px solid rgba(var(--zone-color-rgb, 62, 184, 255), 0.45); border-radius: 14px; padding: 12px; background: linear-gradient(145deg, rgba(var(--zone-color-rgb, 62, 184, 255), 0.12), #fcfeff 55%); box-shadow: inset 0 0 0 1px rgba(var(--zone-color-rgb, 62, 184, 255), 0.16); }
      .relay-header, .relay-states, .relay-actions { display: flex; justify-content: space-between; gap: 10px; align-items: center; }
      .relay-states { font-size: 0.88rem; color: #476070; margin: 8px 0 12px; }
      .status-pill { font-size: 0.7rem; padding: 4px 8px; border-radius: 999px; font-weight: 700; } .status-mismatch { background: #ffe4ea; color: #b42345; } .status-synced { background: #dff8ea; color: #18794e; }
      button { border: 1px solid #9cc3da; color: #123; background: #f3f9ff; padding: 7px 11px; border-radius: 10px; cursor: pointer; } button:disabled { opacity: 0.4; cursor: not-allowed; }
      .env-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;margin-top:12px;} .env-card{border:1px solid #d9e5ee;border-radius:12px;padding:12px;background:#fcfeff;} .env-card iframe{width:100%;height:260px;border:0;border-radius:10px;} .env-links{margin:8px 0 0;padding-left:18px;} .sensor-table{width:100%;border-collapse:collapse;margin-top:8px;} .sensor-table th,.sensor-table td,.telemetry-table th,.telemetry-table td{border-bottom:1px solid #d9e5ee;padding:8px 6px;text-align:left;font-size:.85rem;color:#28455a;} .telemetry-table{width:100%;border-collapse:collapse;margin-top:8px;}
      .schedule { margin-top: 14px; padding: 16px; } .timeline{display:grid;gap:8px;margin:12px 0 16px;} .timeline-row{display:grid;grid-template-columns:170px 1fr;align-items:center;gap:10px;} .timeline-zone{font-weight:600;color:#204055;}
      .timeline-track{height:28px;background:repeating-linear-gradient(to right,#edf2f7,#edf2f7 7.6%,#f8fafc 7.6%,#f8fafc 8.33%);border:1px solid #d8e3eb;border-radius:999px;position:relative;overflow:hidden;} .timeline-block{position:absolute;top:2px;height:22px;border-radius:999px;background:linear-gradient(90deg,#16a34a,#22c55e);color:white;font-size:.75rem;display:flex;align-items:center;padding:0 10px;white-space:nowrap;}
      .schedule form { display: grid; grid-template-columns: repeat(2, minmax(0,1fr)); gap: 10px; } label { display: flex; flex-direction: column; font-size: 0.85rem; color: #395267; }
      input { margin-top: 6px; border-radius: 10px; border: 1px solid #c7d8e5; background: #fff; color: #213547; padding: 8px; } .raw-schedules{color:#688093;font-size:.85rem;}
      @media (max-width: 900px) { .layout { grid-template-columns: 1fr; } .relay-grid, .schedule form { grid-template-columns: 1fr; } .timeline-row{grid-template-columns:1fr;} .hero{display:block;} }
    </style>
  </head>
  <body>
    <div class="shell">
      <section class="panel hero">
        <h1>Castle Hills Garden Manager</h1>
        <p class="timestamp">Current server time (UTC): <strong id="server-time">${new Date().toISOString()}</strong></p>
        <p class="timestamp">State data refresh: every 1 second · Active zones glowing on map.</p>
      </section>
      <div class="layout">
        <section class="panel map-wrap">
          <h2>Garden Zone Map</h2>
          <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 295.743 295.482" role="img" aria-label="Garden watering zones">
            <g class="linework"><polyline points="10,266.311 251.526,274.667"/><line x1="285.743" y1="285.482" x2="282.082" y2="191.277"/><polyline points="282.082,191.277 276.705,168.862 277.246,95.213 277.7,15.387"/><polyline points="13.295,258.387 35.211,10 37.872,11.914 276.96,14.582"/></g>
            <polygon id="zone-1" class="zone zone-theme-4 ${activeZoneIds.includes('zone-1 zone-2') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-1 zone-2')}" points="127.534,159.189 124.128,239.478 15.618,232.055 22.68,152.017 127.534,159.189"/>
            <polygon id="zone-2" class="zone zone-theme-4 ${activeZoneIds.includes('zone-1 zone-2') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-1 zone-2')}" points="146.876,95.99 134.762,166.15 198.935,169.661 205.581,96.842 146.876,95.99"/>
            <polygon id="zone-3" class="zone zone-theme-1 ${activeZoneIds.includes('zone-3') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-3')}" points="205.581,96.842 264.287,97.694 263.108,173.173 198.935,169.661 205.581,96.842"/>
            <polygon id="zone-4" class="zone zone-theme-5 ${activeZoneIds.includes('zone-4') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-4')}" points="32,46.388 46.493,52.635 95.676,139.247 128.342,140.128 127.534,159.189 22.68,152.017 32,46.388"/>
            <polygon id="zone-5" class="zone zone-theme-3 ${activeZoneIds.includes('zone-5') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-5')}" points="152.903,89.893 152.691,13.195 50.949,12.06 136.836,89.869 152.903,89.893"/>
            <polygon id="zone-6" class="zone zone-theme-2 ${activeZoneIds.includes('zone-6') ? 'zone-active' : ''}" data-active="${activeZoneIds.includes('zone-6')}" points="249.304,90.041 249.095,14.271 152.691,13.195 152.903,89.893 249.304,90.041"/>
          </svg>
        </section>
        <section class="panel relay-section">
          <h2>Zones (scheduled irrigation)</h2>
          <ul id="relay-grid" class="relay-grid">${relayMarkup}</ul>
          <h3>Master / Spigots</h3>
          <p>Master valve: <strong>${(state.reportedRelayState[MASTER_VALVE_CHANNEL - 1]?.state || 'off').toUpperCase()}</strong></p>
          <p>Spigot run: <strong>${state.spigotRun.active ? 'active' : 'inactive'}</strong> (${state.spigotRun.remainingSeconds}s remaining)</p>
          <form method="post" action="/gui/spigots/run">
            <label>Spigot minutes <input name="minutes" type="number" min="1" max="240" value="15" /></label>
            <button type="submit">Run Spigots</button>
          </form>
          <form method="post" action="/gui/spigots/stop">
            <button type="submit">Stop Spigots</button>
          </form>
        </section>
      </div>
      <section class="panel schedule">
        <h2>Environmental Monitoring (${GARDEN_LOCATION.lat}, ${GARDEN_LOCATION.lon})</h2>
        <div class="env-grid">
          <article class="env-card">
            <h3>Weather Radar</h3>
            <iframe title="Weather radar near the garden" src="${envFeeds.radarEmbedUrl}"></iframe>
            <ul class="env-links"><li><a href="${envFeeds.nsslRadarUrl}" target="_blank" rel="noreferrer">NOAA MRMS radar product viewer</a></li></ul>
          </article>
          <article class="env-card">
            <h3>Satellite</h3>
            <iframe title="Satellite imagery near the garden" src="${envFeeds.satelliteEmbedUrl}"></iframe>
            <ul class="env-links"><li><a href="${envFeeds.noaaSatelliteUrl}" target="_blank" rel="noreferrer">NOAA GOES-17 Pacific Northwest imagery</a></li></ul>
          </article>
          <article class="env-card">
            <h3>Microcontroller Weather Sensors</h3>
            <div id="sensor-data">${sensorDataMarkup}</div>
          </article>
          <article class="env-card">
            <h3>Device Telemetry</h3>
            <div id="device-telemetry">${telemetryMarkup}</div>
          </article>
          <article class="env-card">
            <h3>Lidar / Elevation</h3>
            <p>Open the USGS National Map viewer centered on the garden location for 3DEP elevation and lidar-derived layers.</p>
            <ul class="env-links"><li><a href="${envFeeds.lidarMapUrl}" target="_blank" rel="noreferrer">USGS National Map (3DEP)</a></li></ul>
          </article>
        </div>
      </section>
      <section class="panel schedule">
        <h2>Schedules</h2>
        <p>Schedules may only use zones 1-5. Channel 6 is master valve/spigots.</p>
        <div id="schedule-timeline">${scheduleTimelineMarkup}</div><details class="raw-schedules"><summary>Raw schedule list</summary><div id="raw-schedules">${schedulesMarkup}</div></details>
        <h3>Update schedules</h3>
        <form method="post" action="/gui/schedules">
          <label>Zone <input name="zone" value="${defaultSchedule.zone || ''}" required /></label>
          <label>Channel <input name="channel" type="number" min="1" max="${ZONE_CHANNELS}" value="${defaultSchedule.channel || 1}" required /></label>
          <label>Start Time <input name="startTime" value="${defaultSchedule.startTime || '06:00'}" required /></label>
          <label>Duration (seconds) <input name="durationSeconds" type="number" min="1" value="${defaultSchedule.durationSeconds || 900}" required /></label>
          <button type="submit">Save schedule</button>
        </form>
      </section>
    </div>
    <script>
      const relayChannelCount = ${RELAY_CHANNELS};
      const zoneShapeIdForChannel = (channel) => ({1:'zone-3',2:'zone-6',3:'zone-5',4:'zone-1 zone-2',5:'zone-4'}[channel] || ('zone-' + channel));
      const zoneShapeIdsForChannel = (channel) => String(zoneShapeIdForChannel(channel)).split(' ');
      const formatScheduleLabel = (schedule) => typeof schedule === 'string'
        ? schedule
        : \`\${schedule.zone} (relay \${schedule.channel}) at \${schedule.startTime} for \${schedule.durationSeconds}s\`;
      function renderFromState(state) {
        const formatTimeSince = (lastSeenAt) => {
          if (!lastSeenAt) return 'never';
          const timestamp = Date.parse(lastSeenAt);
          if (Number.isNaN(timestamp)) return 'unknown';
          const diffSeconds = Math.max(0, Math.floor((Date.now() - timestamp) / 1000));
          if (diffSeconds < 60) return \`\${diffSeconds}s ago\`;
          if (diffSeconds < 3600) return \`\${Math.floor(diffSeconds / 60)}m ago\`;
          if (diffSeconds < 86400) return \`\${Math.floor(diffSeconds / 3600)}h ago\`;
          return \`\${Math.floor(diffSeconds / 86400)}d ago\`;
        };
        const activeZoneIds = new Set((state.reportedRelays || [])
          .filter((relay) => relay.state === 'on')
          .flatMap((relay) => zoneShapeIdsForChannel(relay.channel)));
        const relayGrid = document.getElementById('relay-grid');
        relayGrid.innerHTML = (state.desiredRelays || []).filter((relay) => relay.channel <= ${ZONE_CHANNELS}).map((desiredRelay) => {
          const reportedRelay = (state.reportedRelays || [])[desiredRelay.channel - 1] || { state: 'unknown' };
          const hasMismatch = desiredRelay.state !== reportedRelay.state;
          const statusClass = hasMismatch ? 'status-mismatch' : 'status-synced';
          return \`<li class="relay-card zone-theme-\${desiredRelay.channel}">
            <div class="relay-header"><span class="relay-title">Zone \${desiredRelay.channel}</span><span class="status-pill \${statusClass}">\${hasMismatch ? 'MISMATCH' : 'SYNCED'}</span></div>
            <div class="relay-states"><span>Desired <strong>\${desiredRelay.state.toUpperCase()}</strong></span><span>Reported <strong>\${reportedRelay.state.toUpperCase()}</strong></span></div>
            <div class="relay-actions"><form method="post" action="/gui/relays/\${desiredRelay.channel}/on"><input name="minutes" type="number" min="1" max="240" value="15" /><button type="submit" \${reportedRelay.state === 'on' ? 'disabled' : ''}>Run</button></form><form method="post" action="/gui/relays/\${desiredRelay.channel}/off"><button type="submit" \${reportedRelay.state === 'off' ? 'disabled' : ''}>Stop</button></form></div>
          </li>\`;
        }).join('');
        for (let channel = 1; channel <= relayChannelCount; channel += 1) {
          const zoneShapeIds = zoneShapeIdsForChannel(channel);
          for (const zoneShapeId of zoneShapeIds) {
            const zoneEl = document.getElementById(zoneShapeId);
            if (!zoneEl) continue;
            const isActive = activeZoneIds.has(zoneShapeId);
            zoneEl.setAttribute('data-active', String(isActive));
            zoneEl.setAttribute('class', \`zone zone-theme-\${channel} \${isActive ? 'zone-active' : ''}\`);
          }
        }
        const schedules = Array.isArray(state.schedules) ? state.schedules : [];
        const parsedSchedules = schedules.filter((schedule) => typeof schedule === 'object' && schedule);
        const scheduleTimeline = document.getElementById('schedule-timeline');
        scheduleTimeline.innerHTML = parsedSchedules.length ? \`<div class="timeline">\${parsedSchedules.map((schedule) => {
          const [hoursText = '0', minutesText = '0'] = String(schedule.startTime || '00:00').split(':');
          const startMinutes = Number.parseInt(hoursText, 10) * 60 + Number.parseInt(minutesText, 10);
          const leftPercent = Math.max(0, Math.min((startMinutes / 1440) * 100, 100));
          const widthPercent = Math.max(2, Math.min(((Number(schedule.durationSeconds) || 60) / 86400) * 100, 100 - leftPercent));
          return \`<div class="timeline-row"><span class="timeline-zone">\${schedule.zone}</span><div class="timeline-track"><span class="timeline-block" style="left:\${leftPercent}%;width:\${widthPercent}%">\${schedule.startTime} · \${schedule.durationSeconds}s</span></div></div>\`;
        }).join('')}</div>\` : '<p>No schedules configured.</p>';
        const rawSchedules = document.getElementById('raw-schedules');
        rawSchedules.innerHTML = schedules.length ? \`<ul>\${schedules.map((schedule) => \`<li>\${formatScheduleLabel(schedule)}</li>\`).join('')}</ul>\` : '';
        const sensorContainer = document.getElementById('sensor-data');
        const readings = state.latestSensorData && Array.isArray(state.latestSensorData.sensorData)
          ? state.latestSensorData.sensorData
          : ((state.deviceTelemetry && Array.isArray(state.deviceTelemetry.sensorData)) ? state.deviceTelemetry.sensorData : []);
        if (sensorContainer) {
          if (!readings.length) {
            sensorContainer.innerHTML = '<p id="sensor-empty">No microcontroller weather sensor data reported yet.</p>';
          } else {
            const sensorRows = readings.map((reading) => \`<tr><td>\${reading.type || 'unknown'}</td><td>\${reading.value ?? 'n/a'}</td><td>\${reading.unit || ''}</td><td>\${reading.source || 'firmware'}</td></tr>\`).join('');
            sensorContainer.innerHTML = \`<table class="sensor-table" id="sensor-table"><thead><tr><th>Metric</th><th>Value</th><th>Unit</th><th>Source</th></tr></thead><tbody>\${sensorRows}</tbody></table>\`;
          }
        }
        const telemetryContainer = document.getElementById('device-telemetry');
        const telemetry = state.deviceTelemetry;
        if (telemetryContainer) {
          if (!telemetry) {
            telemetryContainer.innerHTML = '<p id="telemetry-empty">No device telemetry reported yet.</p>';
          } else {
            const pairs = [
              ['Device ID', telemetry.deviceId || 'n/a'],
              ['Firmware', telemetry.firmwareVersion || 'n/a'],
              ['Clock Valid', String(Boolean(telemetry.clockValid))],
              ['Last Command ID', telemetry.lastCommandId || 'n/a'],
              ['Current Run', telemetry.currentRun ? JSON.stringify(telemetry.currentRun) : 'none'],
              ['Target Location', telemetry.targetLocation ? JSON.stringify(telemetry.targetLocation) : 'n/a'],
              ['Last Seen (UTC)', telemetry.lastSeenAt || 'n/a'],
              ['Last Seen', formatTimeSince(telemetry.lastSeenAt)]
            ];
            telemetryContainer.innerHTML = \`<table class="telemetry-table" id="telemetry-table"><tbody>\${pairs.map(([label, value]) => \`<tr><th>\${label}</th><td>\${value}</td></tr>\`).join('')}</tbody></table>\`;
          }
        }
        const serverTime = document.getElementById('server-time');
        serverTime.textContent = state.serverTime || new Date().toISOString();
      }
      async function refreshState() {
        const response = await fetch('/gui/state', { cache: 'no-store' });
        if (!response.ok) return;
        renderFromState(await response.json());
      }
      setInterval(refreshState, 1000);
    </script>
  </body>
</html>`);
;
  });

  app.get('/gui/state', requireGuiAuth, (_req, res) => {
    res.json({
      zoneChannels: ZONE_CHANNELS,
      relayChannels: RELAY_CHANNELS,
      masterValveChannel: MASTER_VALVE_CHANNEL,
      desiredRelays: state.desiredRelayState,
      reportedRelays: state.reportedRelayState,
      masterValveState: state.reportedRelayState[MASTER_VALVE_CHANNEL - 1],
      spigotRun: state.spigotRun,
      schedules: state.schedules,
      serverTime: new Date().toISOString(),
      latestSensorData: state.latestSensorData,
      deviceTelemetry: state.deviceTelemetry
    });
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

    const minutes = Number.parseInt(req.body.minutes, 10);
    const durationSeconds = Number.isInteger(minutes) && minutes > 0 ? minutes * 60 : DEFAULT_ON_DURATION_SECONDS;
    let command;
    if (channel === MASTER_VALVE_CHANNEL) {
      command = createSpigotCommand({ action, durationSeconds, requestedBy: 'gui-web' });
      state.spigotRun = action === 'on'
        ? { active: true, remainingSeconds: durationSeconds, updatedAt: new Date().toISOString() }
        : { active: false, remainingSeconds: 0, updatedAt: new Date().toISOString() };
    } else {
      command = createRelayCommand({ channel, action, requestedBy: 'gui-web' });
      if (action === 'on') {
        command.durationSeconds = durationSeconds;
      }
    }
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

  app.post('/gui/spigots/run', requireGuiAuth, (req, res) => {
    const minutes = Number.parseInt(req.body.minutes, 10);
    const durationSeconds = Number.isInteger(minutes) && minutes > 0 ? minutes * 60 : DEFAULT_ON_DURATION_SECONDS;
    const command = createSpigotCommand({ action: 'on', durationSeconds, requestedBy: 'gui-web' });
    state.desiredRelayState[MASTER_VALVE_CHANNEL - 1].state = 'on';
    state.spigotRun = { active: true, remainingSeconds: durationSeconds, updatedAt: new Date().toISOString() };
    state.queue.push(command);
    wakePendingPolls();
    return res.redirect(303, '/gui');
  });

  app.post('/gui/spigots/stop', requireGuiAuth, (_req, res) => {
    const command = createSpigotCommand({ action: 'off', durationSeconds: 0, requestedBy: 'gui-web' });
    state.desiredRelayState[MASTER_VALVE_CHANNEL - 1].state = 'off';
    state.spigotRun = { active: false, remainingSeconds: 0, updatedAt: new Date().toISOString() };
    state.queue.push(command);
    wakePendingPolls();
    return res.redirect(303, '/gui');
  });

  if (config.enableWeatherRefresh !== false) {
    const WEATHER_REFRESH_INTERVAL_MS = 15 * 60 * 1000;

    setInterval(() => {
      refreshWeatherDatasets().catch((error) => {
        console.error('Weather refresh failed:', error.message);
      });
    }, WEATHER_REFRESH_INTERVAL_MS);

    setTimeout(() => {
      refreshWeatherDatasets().catch((error) => {
        console.error('Initial weather refresh failed:', error.message);
      });
    }, 2000);
  }

  app.get('/openapi.json', (_req, res) => {
    const specPath = path.join(__dirname, '..', 'openapi.yaml');
    const raw = fs.readFileSync(specPath, 'utf8');
    const spec = yaml.load(raw);
    res.json(spec);
  });

  return { app, state };
}

module.exports = { createApp, createState, ZONE_CHANNELS, RELAY_CHANNELS, MASTER_VALVE_CHANNEL };
