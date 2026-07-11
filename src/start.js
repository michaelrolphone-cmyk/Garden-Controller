const express = require('express');
const appModule = require('./app');

const MASTER_VALVE_CHANNEL = 6;
const ZONE_CHANNELS = 5;
const MAX_DAILY_SCHEDULES = 64;
const RUN_WINDOW_START_MINUTES = 4 * 60;
const RUN_WINDOW_END_MINUTES = 20 * 60;

function timeToMinutes(value) {
  const match = /^([01]\d|2[0-3]):([0-5]\d)$/.exec(String(value || '').trim());
  return match ? Number(match[1]) * 60 + Number(match[2]) : null;
}

function normalizeSchedule(schedule, index) {
  const channel = Number(schedule.channel);
  return {
    id: Number.isInteger(Number(schedule.id)) ? Number(schedule.id) : index,
    channel,
    zone: channel === MASTER_VALVE_CHANNEL
      ? 'Spigots'
      : String(schedule.zone || `Zone ${channel}`).trim(),
    enabled: schedule.enabled === undefined ? true : Boolean(schedule.enabled),
    startTime: String(schedule.startTime || '').trim(),
    durationSeconds: Number(schedule.durationSeconds),
    ...(schedule.daysMask !== undefined ? { daysMask: Number(schedule.daysMask) } : {}),
    ...(schedule.days !== undefined ? { days: schedule.days } : {})
  };
}

function validateSchedules(schedules) {
  if (!Array.isArray(schedules)) return 'Schedules must be an array.';
  if (schedules.length > MAX_DAILY_SCHEDULES) {
    return `Too many schedule entries. Maximum is ${MAX_DAILY_SCHEDULES}.`;
  }

  const intervals = [];
  for (const schedule of schedules) {
    const channel = Number(schedule.channel);
    const startMinutes = timeToMinutes(schedule.startTime);
    const durationSeconds = Number(schedule.durationSeconds);
    const name = channel === MASTER_VALVE_CHANNEL
      ? 'Spigots'
      : String(schedule.zone || `Zone ${channel}`);
    const enabled = schedule.enabled === undefined ? true : Boolean(schedule.enabled);

    if (!Number.isInteger(channel) || channel < 1 || channel > MASTER_VALVE_CHANNEL) {
      return `${name} has an invalid channel. Use zones 1-5 or channel 6 for Spigots.`;
    }
    if (!name.trim()) return 'Every schedule needs a name.';
    if (startMinutes === null) return `${name} has an invalid start time.`;
    if (!Number.isInteger(durationSeconds) || durationSeconds <= 0) {
      return `${name} has an invalid duration.`;
    }

    if (enabled) {
      const endMinutes = startMinutes + durationSeconds / 60;
      if (startMinutes < RUN_WINDOW_START_MINUTES || endMinutes > RUN_WINDOW_END_MINUTES) {
        return `${name} at ${schedule.startTime} runs outside the allowed 4:00am-8:00pm window.`;
      }
      intervals.push({ name, startMinutes, endMinutes });
    }
  }

  intervals.sort((a, b) => a.startMinutes - b.startMinutes || a.endMinutes - b.endMinutes);
  for (let index = 1; index < intervals.length; index += 1) {
    const previous = intervals[index - 1];
    const current = intervals[index];
    if (current.startMinutes < previous.endMinutes) {
      return `${current.name} overlaps ${previous.name}.`;
    }
  }

  return null;
}

function parseGuiSchedules(body) {
  const rows = {};
  for (const [key, value] of Object.entries(body || {})) {
    const match = key.match(/^schedule\[(\d+)\]\[(\w+)\]$/);
    if (!match) continue;
    rows[match[1]] = rows[match[1]] || {};
    rows[match[1]][match[2]] = value;
  }

  if (!Object.keys(rows).length && body?.channel && body?.zone) {
    rows[0] = {
      channel: body.channel,
      zone: body.zone,
      startTime: body.startTime,
      durationMinutes: body.durationMinutes,
      enabled: 'on'
    };
  }

  return Object.keys(rows)
    .sort((a, b) => Number(a) - Number(b))
    .map((key, index) => {
      const row = rows[key];
      return normalizeSchedule({
        id: row.id,
        channel: Number(row.channel),
        zone: row.zone,
        enabled: row.enabled !== undefined,
        startTime: row.startTime,
        durationSeconds: Number(row.durationMinutes) * 60
      }, index);
    });
}

function guiBodyFromSchedules(schedules) {
  const body = {};
  schedules.forEach((schedule, index) => {
    body[`schedule[${index}][id]`] = String(schedule.id ?? index);
    if (schedule.enabled !== false) body[`schedule[${index}][enabled]`] = 'on';
    body[`schedule[${index}][zone]`] = schedule.zone;
    body[`schedule[${index}][channel]`] = String(schedule.channel);
    body[`schedule[${index}][startTime]`] = schedule.startTime;
    body[`schedule[${index}][durationMinutes]`] = String(
      Math.max(1, Math.round(schedule.durationSeconds / 60))
    );
  });
  return body;
}

function findRoute(app, routePath, method) {
  return app._router?.stack
    .find((layer) => layer.route?.path === routePath && layer.route.methods?.[method])
    ?.route;
}

function replaceAuthenticatedRoute(route, handler) {
  if (!route || !route.stack?.length) {
    throw new Error('Expected authenticated route was not found.');
  }

  const authLayer = route.stack[0];
  const handlerLayer = route.stack[route.stack.length - 1];
  handlerLayer.handle = handler;
  route.stack = [authLayer, handlerLayer];
  return authLayer.handle;
}

function installScheduleRouteSupport(app, state) {
  let spigotSchedulesAuthoritative = false;
  const apiRoute = findRoute(app, '/api/schedules', 'post');
  const originalApiHandler = apiRoute.stack[apiRoute.stack.length - 1].handle;

  const apiAuth = replaceAuthenticatedRoute(apiRoute, (req, res, next) => {
    const fullSchedules = (req.body?.schedules || []).map(normalizeSchedule);
    const error = validateSchedules(fullSchedules);
    if (error) {
      return res.status(400).json({
        error: 'Invalid schedule timing',
        detail: error,
        constraints: {
          noOverlappingRuns: true,
          allowedRunWindow: '04:00-20:00',
          scheduledChannels: '1-6'
        }
      });
    }

    spigotSchedulesAuthoritative = true;
    const zoneSchedules = fullSchedules.filter((schedule) => schedule.channel <= ZONE_CHANNELS);
    const originalBody = req.body;
    const originalJson = res.json.bind(res);

    req.body = { ...originalBody, schedules: zoneSchedules };
    res.json = (payload) => {
      state.schedules = fullSchedules.map((schedule) => ({ ...schedule }));
      if (payload && typeof payload === 'object') payload.schedules = state.schedules;
      return originalJson(payload);
    };

    const result = originalApiHandler(req, res, next);
    state.schedules = fullSchedules.map((schedule) => ({ ...schedule }));
    req.body = originalBody;
    return result;
  });

  const guiRoute = findRoute(app, '/gui/schedules', 'post');
  const originalGuiHandler = guiRoute.stack[guiRoute.stack.length - 1].handle;

  replaceAuthenticatedRoute(guiRoute, (req, res, next) => {
    const fullSchedules = parseGuiSchedules(req.body);
    const error = validateSchedules(fullSchedules);
    if (error) {
      return res.status(400).type('text').send(
        `${error}\n\nSchedules must not overlap and must stay between 4:00am and 8:00pm.`
      );
    }

    const originalBody = req.body;
    const originalRedirect = res.redirect.bind(res);
    spigotSchedulesAuthoritative = true;
    const zoneSchedules = fullSchedules.filter((schedule) => schedule.channel <= ZONE_CHANNELS);

    req.body = guiBodyFromSchedules(zoneSchedules);
    res.redirect = (...args) => {
      state.schedules = fullSchedules.map((schedule) => ({ ...schedule }));
      return originalRedirect(...args);
    };

    const result = originalGuiHandler(req, res, next);
    state.schedules = fullSchedules.map((schedule) => ({ ...schedule }));
    req.body = originalBody;
    return result;
  });

  const apiDeleteRoute = findRoute(app, '/api/schedules/:id', 'delete');
  if (apiDeleteRoute) {
    const originalApiDeleteHandler = apiDeleteRoute.stack[apiDeleteRoute.stack.length - 1].handle;
    replaceAuthenticatedRoute(apiDeleteRoute, (req, res, next) => {
      const originalJson = res.json.bind(res);
      res.json = (payload) => {
        if (res.statusCode < 400) spigotSchedulesAuthoritative = true;
        return originalJson(payload);
      };
      return originalApiDeleteHandler(req, res, next);
    });
  }

  const guiDeleteRoute = findRoute(app, '/gui/schedules/:id/delete', 'post');
  if (guiDeleteRoute) {
    const originalGuiDeleteHandler = guiDeleteRoute.stack[guiDeleteRoute.stack.length - 1].handle;
    replaceAuthenticatedRoute(guiDeleteRoute, (req, res, next) => {
      const originalRedirect = res.redirect.bind(res);
      res.redirect = (...args) => {
        spigotSchedulesAuthoritative = true;
        return originalRedirect(...args);
      };
      return originalGuiDeleteHandler(req, res, next);
    });
  }

  const firmwareSchedulesRoute = findRoute(app, '/api/microcontroller/schedules', 'post');
  const originalFirmwareSchedulesHandler =
    firmwareSchedulesRoute.stack[firmwareSchedulesRoute.stack.length - 1].handle;

  replaceAuthenticatedRoute(firmwareSchedulesRoute, (req, res, next) => {
    const incoming = Array.isArray(req.body?.schedules)
      ? req.body.schedules.map(normalizeSchedule)
      : [];
    const incomingSpigots = incoming.filter(
      (schedule) => schedule.channel === MASTER_VALVE_CHANNEL
    );
    const includesSpigotSchedules = req.body?.includesSpigotSchedules === true;
    if (includesSpigotSchedules) {
      const error = validateSchedules(incoming);
      if (error) {
        return res.status(400).json({ error: 'Invalid schedule timing', detail: error });
      }
      spigotSchedulesAuthoritative = true;
    }
    const preservedSpigots = includesSpigotSchedules
      ? incomingSpigots
      : state.schedules.filter((schedule) => schedule.channel === MASTER_VALVE_CHANNEL);
    const originalBody = req.body;
    const originalJson = res.json.bind(res);

    req.body = {
      ...originalBody,
      schedules: incoming.filter((schedule) => schedule.channel <= ZONE_CHANNELS)
    };

    res.json = (payload) => {
      state.schedules = [
        ...state.schedules.filter((schedule) => schedule.channel <= ZONE_CHANNELS),
        ...preservedSpigots
      ];
      if (state.deviceTelemetry) state.deviceTelemetry.schedules = state.schedules;
      if (payload && typeof payload === 'object') payload.schedules = state.schedules;
      return originalJson(payload);
    };

    const result = originalFirmwareSchedulesHandler(req, res, next);
    state.schedules = [
      ...state.schedules.filter((schedule) => schedule.channel <= ZONE_CHANNELS),
      ...preservedSpigots
    ];
    req.body = originalBody;
    return result;
  });

  const firmwareStateRoute = findRoute(app, '/api/microcontroller/state', 'post');
  const originalFirmwareStateHandler =
    firmwareStateRoute.stack[firmwareStateRoute.stack.length - 1].handle;

  replaceAuthenticatedRoute(firmwareStateRoute, (req, res, next) => {
    const preservedSpigots = state.schedules.filter(
      (schedule) => schedule.channel === MASTER_VALVE_CHANNEL
    );
    const originalJson = res.json.bind(res);

    res.json = (payload) => {
      state.schedules = [
        ...state.schedules.filter((schedule) => schedule.channel <= ZONE_CHANNELS),
        ...preservedSpigots
      ];
      if (state.deviceTelemetry) state.deviceTelemetry.schedules = state.schedules;
      return originalJson(payload);
    };

    const result = originalFirmwareStateHandler(req, res, next);
    state.schedules = [
      ...state.schedules.filter((schedule) => schedule.channel <= ZONE_CHANNELS),
      ...preservedSpigots
    ];
    if (state.deviceTelemetry) state.deviceTelemetry.schedules = state.schedules;
    return result;
  });

  app.get('/api/firmware/spigot-schedules', apiAuth, (_req, res) => {
    res.json({
      authoritative: spigotSchedulesAuthoritative,
      schedules: state.schedules
        .filter((schedule) => Number(schedule.channel) === MASTER_VALVE_CHANNEL)
        .map((schedule, index) => normalizeSchedule(schedule, index))
    });
  });
}

function installGuiSpigotSchedulePatch() {
  const originalSend = express.response.send;
  if (originalSend.__spigotScheduleGuiPatchInstalled) return;

  express.response.send = function sendWithSpigotScheduleGuiPatch(body) {
    if (this.req?.path === '/gui' && typeof body === 'string') {
      const channelSixScript = `<script>
        (() => {
          const enableSpigotScheduleChannels = () => {
            document.querySelectorAll('input[name*="[channel]"]').forEach((input) => {
              input.max = '6';
              input.title = 'Zones 1-5 or channel 6 for Spigots';
            });
          };
          enableSpigotScheduleChannels();
          new MutationObserver(enableSpigotScheduleChannels).observe(document.body, {
            childList: true,
            subtree: true
          });
        })();
      </script>`;

      body = body
        .replace(
          'Edit the complete schedule list and save it as one update.',
          'Edit the complete schedule list and save it as one update. Channel 6 schedules the Spigots.'
        )
        .replace('</body>', `${channelSixScript}</body>`);
    }
    return originalSend.call(this, body);
  };

  express.response.send.__spigotScheduleGuiPatchInstalled = true;
}

installGuiSpigotSchedulePatch();

const originalCreateApp = appModule.createApp;
appModule.createApp = function createAppWithSpigotSchedules(config) {
  const result = originalCreateApp(config);
  installScheduleRouteSupport(result.app, result.state);
  return result;
};

require('./server');
