const express = require('express');
const fs = require('fs');
const path = require('path');

const ZONE_CHANNELS = 5;
const MAX_DAILY_SCHEDULES = 64;
const RUN_WINDOW_START_MINUTES = 4 * 60;
const RUN_WINDOW_END_MINUTES = 20 * 60;
const PRESETS_PATH = path.join(__dirname, '..', 'data', 'schedule-presets.json');

const GUI_REFRESH_INTERVAL_SNIPPET = '      setInterval(refreshState, 1000);';
const GUI_MANUAL_RUN_INPUT_PATCH_MARKER = '// Manual run input persistence patch';
const GUI_PRESETS_MARKER = '<!-- Schedule preset selector patch -->';

function timeToMinutes(value) {
  const match = /^([01]\d|2[0-3]):([0-5]\d)$/.exec(String(value || '').trim());
  return match ? Number(match[1]) * 60 + Number(match[2]) : null;
}

function minutesToTime(value) {
  const minutes = Math.max(0, Math.min((24 * 60) - 1, value));
  return `${String(Math.floor(minutes / 60)).padStart(2, '0')}:${String(minutes % 60).padStart(2, '0')}`;
}

function minutesToLabel(value) {
  const minutes = Math.max(0, Math.min(24 * 60, Math.ceil(value)));
  const hour = Math.floor(minutes / 60);
  const minute = minutes % 60;
  return `${hour % 12 || 12}:${String(minute).padStart(2, '0')}${hour >= 12 ? 'pm' : 'am'}`;
}

function expandPresetPasses(passes) {
  const schedules = [];
  for (const pass of Array.isArray(passes) ? passes : []) {
    const startMinutes = timeToMinutes(pass.startTime);
    const durationSeconds = Number.isInteger(Number(pass.durationSeconds))
      ? Number(pass.durationSeconds)
      : Number(pass.durationMinutes) * 60;
    const offsetMinutes = Number.isInteger(Number(pass.offsetMinutes))
      ? Number(pass.offsetMinutes)
      : Math.max(1, Math.ceil(durationSeconds / 60));
    const zones = Array.isArray(pass.zones) && pass.zones.length
      ? pass.zones.map(Number).filter(Number.isInteger)
      : Array.from({ length: ZONE_CHANNELS }, (_, index) => index + 1);

    if (startMinutes === null || !Number.isInteger(durationSeconds) || durationSeconds <= 0) {
      schedules.push({ channel: NaN, zone: 'Invalid preset pass', enabled: true, startTime: String(pass.startTime || ''), durationSeconds: NaN });
      continue;
    }

    zones.forEach((channel, zoneIndex) => {
      schedules.push({
        id: schedules.length,
        channel,
        zone: `Zone ${channel}`,
        enabled: pass.enabled === undefined ? true : Boolean(pass.enabled),
        startTime: minutesToTime(startMinutes + (zoneIndex * offsetMinutes)),
        durationSeconds
      });
    });
  }
  return schedules;
}

function normalizePreset(rawPreset, index) {
  const schedules = Array.isArray(rawPreset.schedules)
    ? rawPreset.schedules.map((schedule, scheduleIndex) => ({
      id: Number.isInteger(Number(schedule.id)) ? Number(schedule.id) : scheduleIndex,
      channel: Number(schedule.channel),
      zone: String(schedule.zone || `Zone ${schedule.channel}`),
      enabled: schedule.enabled === undefined ? true : Boolean(schedule.enabled),
      startTime: String(schedule.startTime || ''),
      durationSeconds: Number(schedule.durationSeconds)
    }))
    : expandPresetPasses(rawPreset.passes);

  return {
    id: String(rawPreset.id || `preset-${index}`),
    name: String(rawPreset.name || `Preset ${index + 1}`),
    season: String(rawPreset.season || 'Seasonal'),
    description: String(rawPreset.description || ''),
    schedules
  };
}

function validateSchedules(schedules) {
  if (!Array.isArray(schedules)) return { ok: false, message: 'Schedules must be an array.' };
  if (schedules.length > MAX_DAILY_SCHEDULES) return { ok: false, message: `Too many schedule entries. Maximum is ${MAX_DAILY_SCHEDULES}.` };

  const intervals = [];
  for (const [index, schedule] of schedules.entries()) {
    const channel = Number(schedule.channel);
    const startMinutes = timeToMinutes(schedule.startTime);
    const durationSeconds = Number(schedule.durationSeconds);
    const zone = String(schedule.zone || `Zone ${channel}`);
    const enabled = schedule.enabled === undefined ? true : Boolean(schedule.enabled);

    if (!Number.isInteger(channel) || channel < 1 || channel > ZONE_CHANNELS) {
      return { ok: false, message: `${zone} has an invalid channel. Use zones 1-${ZONE_CHANNELS}.` };
    }
    if (!zone.trim()) return { ok: false, message: `Schedule row ${index + 1} needs a zone name.` };
    if (startMinutes === null) return { ok: false, message: `${zone} has an invalid start time.` };
    if (!Number.isInteger(durationSeconds) || durationSeconds <= 0) return { ok: false, message: `${zone} has an invalid duration.` };

    if (enabled) {
      const endMinutes = startMinutes + (durationSeconds / 60);
      if (startMinutes < RUN_WINDOW_START_MINUTES || endMinutes > RUN_WINDOW_END_MINUTES) {
        return { ok: false, message: `${zone} at ${schedule.startTime} would run outside the allowed 4:00am-8:00pm window.` };
      }
      intervals.push({ startMinutes, endMinutes, zone, startTime: schedule.startTime });
    }
  }

  intervals.sort((a, b) => a.startMinutes - b.startMinutes || a.endMinutes - b.endMinutes);
  for (let index = 1; index < intervals.length; index += 1) {
    const previous = intervals[index - 1];
    const current = intervals[index];
    if (current.startMinutes < previous.endMinutes) {
      return { ok: false, message: `${current.zone} at ${current.startTime} overlaps ${previous.zone}, which runs until ${minutesToLabel(previous.endMinutes)}.` };
    }
  }

  return { ok: true };
}

function loadSchedulePresets() {
  let raw;
  try {
    raw = JSON.parse(fs.readFileSync(PRESETS_PATH, 'utf8'));
  } catch (error) {
    console.error(`Failed to load schedule presets from ${PRESETS_PATH}:`, error.message);
    return [];
  }

  return (Array.isArray(raw.presets) ? raw.presets : [])
    .map(normalizePreset)
    .filter((preset) => {
      const validation = validateSchedules(preset.schedules);
      if (!validation.ok) console.error(`Skipping invalid preset "${preset.name}": ${validation.message}`);
      return validation.ok;
    });
}

function parseGuiSchedules(body) {
  const rowsByIndex = {};
  for (const [key, value] of Object.entries(body || {})) {
    const match = key.match(/^schedule\[(\d+)\]\[(\w+)\]$/);
    if (!match) continue;
    rowsByIndex[match[1]] = rowsByIndex[match[1]] || {};
    rowsByIndex[match[1]][match[2]] = value;
  }

  if (!Object.keys(rowsByIndex).length && body?.channel && body?.zone) {
    rowsByIndex[0] = { channel: body.channel, zone: body.zone, startTime: body.startTime, durationMinutes: body.durationMinutes, enabled: 'on' };
  }

  return Object.keys(rowsByIndex)
    .sort((a, b) => Number(a) - Number(b))
    .map((key, index) => {
      const row = rowsByIndex[key];
      const durationMinutes = Number(row.durationMinutes);
      return {
        id: Number.isInteger(Number(row.id)) ? Number(row.id) : index,
        channel: Number(row.channel),
        zone: String(row.zone || '').trim(),
        enabled: row.enabled !== undefined,
        startTime: String(row.startTime || '').trim(),
        durationSeconds: Number.isInteger(durationMinutes) ? durationMinutes * 60 : NaN
      };
    });
}

function installScheduleValidation() {
  const originalPost = express.application.post;
  if (originalPost.__gardenScheduleValidationInstalled) return;

  function insertAfterAuth(handlers, validator) {
    return handlers.length ? [handlers[0], validator, ...handlers.slice(1)] : [validator];
  }

  express.application.post = function postWithScheduleValidation(routePath, ...handlers) {
    if (routePath === '/gui/schedules') {
      return originalPost.call(this, routePath, ...insertAfterAuth(handlers, (req, res, next) => {
        const validation = validateSchedules(parseGuiSchedules(req.body));
        if (!validation.ok) return res.status(400).type('text').send(`${validation.message}\n\nSchedules must not overlap, and enabled runs must stay between 4:00am and 8:00pm.`);
        return next();
      }));
    }

    if (routePath === '/api/schedules') {
      return originalPost.call(this, routePath, ...insertAfterAuth(handlers, (req, res, next) => {
        const validation = validateSchedules(req.body?.schedules);
        if (!validation.ok) return res.status(400).json({ error: 'Invalid schedule timing', detail: validation.message, constraints: { noOverlappingZones: true, allowedRunWindow: '04:00-20:00' } });
        return next();
      }));
    }

    return originalPost.call(this, routePath, ...handlers);
  };

  express.application.post.__gardenScheduleValidationInstalled = true;
}

function escapeHtml(value) {
  return String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;').replaceAll("'", '&#39;');
}

function presetControlsHtml(presets) {
  const options = presets.length
    ? presets.map((preset, index) => `<option value="${escapeHtml(preset.id)}" ${index === 0 ? 'selected' : ''}>${escapeHtml(preset.name)} · ${escapeHtml(preset.season)}</option>`).join('')
    : '<option value="">No presets available</option>';

  return `${GUI_PRESETS_MARKER}
          <div class="schedule-preset-panel">
            <div class="preset-header">
              <div>
                <h3>Weather schedule presets</h3>
                <p class="preset-copy">Pick a deployed weather preset, or keep using the custom schedule editor below. Presets and custom saves are validated so zones never overlap and enabled runs stay between 4:00am and 8:00pm.</p>
              </div>
              <form id="schedule-preset-form" method="post" action="/gui/schedules"></form>
            </div>
            <div class="preset-controls">
              <label>Preset <select id="schedule-preset-select" ${presets.length ? '' : 'disabled'}>${options}</select></label>
              <button type="button" id="schedule-preset-apply" ${presets.length ? '' : 'disabled'}>Apply preset</button>
            </div>
            <p id="schedule-preset-description" class="preset-description"></p>
            <details class="preset-details"><summary>Preview selected preset</summary><ul id="schedule-preset-preview"></ul></details>
          </div>`;
}

function presetCss() {
  return `      .schedule-preset-panel{border:1px solid #cfe0ec;border-radius:14px;padding:14px;margin:12px 0 16px;background:#f8fbff;}
      .preset-header{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;}
      .preset-copy,.preset-description{color:#516a7c;margin:6px 0 0;font-size:.9rem;line-height:1.35;}
      .preset-controls{display:grid;grid-template-columns:minmax(220px,1fr) auto;gap:10px;align-items:end;margin-top:12px;}
      .preset-controls select{margin-top:6px;border-radius:10px;border:1px solid #c7d8e5;background:#fff;color:#213547;padding:8px;width:100%;}
      .preset-details{margin-top:10px;color:#395267;} .preset-details ul{margin:8px 0 0;padding-left:20px;columns:2;}
`;
}

function presetScript(presets) {
  const json = JSON.stringify(presets).replace(/</g, '\\u003c');
  return `      const schedulePresets = ${json};
      const schedulePresetSelect = document.getElementById('schedule-preset-select');
      const schedulePresetApplyButton = document.getElementById('schedule-preset-apply');
      const schedulePresetDescription = document.getElementById('schedule-preset-description');
      const schedulePresetPreview = document.getElementById('schedule-preset-preview');
      const schedulePresetForm = document.getElementById('schedule-preset-form');
      const schedulePresetDurationMinutes = (schedule) => Math.max(1, Math.round((Number(schedule.durationSeconds) || 60) / 60));
      const selectedSchedulePreset = () => schedulePresets.find((preset) => preset.id === schedulePresetSelect?.value);
      const renderSchedulePresetPreview = () => {
        const preset = selectedSchedulePreset();
        if (!preset) return;
        const totalMinutes = preset.schedules.reduce((total, schedule) => total + schedulePresetDurationMinutes(schedule), 0);
        schedulePresetDescription.textContent = preset.schedules.length
          ? preset.description + ' Total active runtime: ' + totalMinutes + ' zone-minutes/day across ' + preset.schedules.length + ' runs.'
          : preset.description + ' This preset clears the active daily schedule.';
        schedulePresetPreview.innerHTML = preset.schedules.length
          ? preset.schedules.slice().sort((a,b)=>String(a.startTime).localeCompare(String(b.startTime))).map((schedule) => '<li>' + schedule.startTime + ' · ' + schedule.zone + ' · ' + schedulePresetDurationMinutes(schedule) + ' min</li>').join('')
          : '<li>No watering runs.</li>';
      };
      const addSchedulePresetHiddenInput = (name, value) => {
        const input = document.createElement('input');
        input.type = 'hidden';
        input.name = name;
        input.value = String(value);
        schedulePresetForm.appendChild(input);
      };
      if (schedulePresetSelect && schedulePresetApplyButton && schedulePresetForm) {
        schedulePresetSelect.addEventListener('change', renderSchedulePresetPreview);
        schedulePresetApplyButton.addEventListener('click', () => {
          const preset = selectedSchedulePreset();
          if (!preset) return;
          schedulePresetForm.innerHTML = '';
          preset.schedules.forEach((schedule, index) => {
            addSchedulePresetHiddenInput('schedule[' + index + '][id]', Number.isInteger(schedule.id) ? schedule.id : index);
            if (schedule.enabled !== false) addSchedulePresetHiddenInput('schedule[' + index + '][enabled]', 'on');
            addSchedulePresetHiddenInput('schedule[' + index + '][zone]', schedule.zone || ('Zone ' + schedule.channel));
            addSchedulePresetHiddenInput('schedule[' + index + '][channel]', schedule.channel);
            addSchedulePresetHiddenInput('schedule[' + index + '][startTime]', schedule.startTime);
            addSchedulePresetHiddenInput('schedule[' + index + '][durationMinutes]', schedulePresetDurationMinutes(schedule));
          });
          schedulePresetForm.submit();
        });
        renderSchedulePresetPreview();
      }
`;
}

function manualRunInputPatchScript() {
  return `      ${GUI_MANUAL_RUN_INPUT_PATCH_MARKER}
      const preserveManualRunInputValues = (renderFn) => function renderWithManualRunInputPersistence(nextState) {
        const relayGridBeforeRender = document.getElementById('relay-grid');
        const activeElement = document.activeElement;
        const activeManualInput = activeElement instanceof HTMLInputElement && activeElement.name === 'minutes' && Boolean(activeElement.closest('#relay-grid form[action^="/gui/relays/"][action$="/on"]'));
        const focusedFormAction = activeManualInput ? activeElement.closest('form').getAttribute('action') : null;
        const manualRunValues = new Map();
        if (relayGridBeforeRender) {
          for (const input of relayGridBeforeRender.querySelectorAll('form[action^="/gui/relays/"][action$="/on"] input[name="minutes"]')) {
            const formAction = input.closest('form')?.getAttribute('action');
            if (formAction) manualRunValues.set(formAction, input.value);
          }
        }
        renderFn(nextState);
        const relayGridAfterRender = document.getElementById('relay-grid');
        if (!relayGridAfterRender) return;
        for (const input of relayGridAfterRender.querySelectorAll('form[action^="/gui/relays/"][action$="/on"] input[name="minutes"]')) {
          const formAction = input.closest('form')?.getAttribute('action');
          if (formAction && manualRunValues.has(formAction)) input.value = manualRunValues.get(formAction);
          if (formAction === focusedFormAction) input.focus({ preventScroll: true });
        }
      };
      renderFromState = preserveManualRunInputValues(renderFromState);
`;
}

function enhanceGuiHtml(body) {
  if (typeof body !== 'string') return body;

  const presets = loadSchedulePresets();
  let html = body;
  if (!html.includes(GUI_PRESETS_MARKER)) {
    html = html.replace('<form method="post" action="/gui/schedules">', `${presetControlsHtml(presets)}
        <form method="post" action="/gui/schedules">`);
    html = html.replace('      @media (max-width: 900px)', `${presetCss()}      @media (max-width: 900px)`);
    html = html.replace('      const relayChannelCount = ', `${presetScript(presets)}      const relayChannelCount = `);
  }

  if (html.includes(GUI_REFRESH_INTERVAL_SNIPPET) && !html.includes(GUI_MANUAL_RUN_INPUT_PATCH_MARKER)) {
    html = html.replace(GUI_REFRESH_INTERVAL_SNIPPET, `${manualRunInputPatchScript()}${GUI_REFRESH_INTERVAL_SNIPPET}`);
  }

  return html;
}

function installGuiHtmlEnhancements() {
  const originalSend = express.response.send;
  if (originalSend.__gardenGuiHtmlEnhancementsInstalled) return;

  express.response.send = function sendWithGuiHtmlEnhancements(body) {
    if (this.req && this.req.path === '/gui') return originalSend.call(this, enhanceGuiHtml(body));
    return originalSend.call(this, body);
  };

  express.response.send.__gardenGuiHtmlEnhancementsInstalled = true;
}

installScheduleValidation();
installGuiHtmlEnhancements();

const { createApp } = require('./app');

const PORT = process.env.PORT || 3000;
const { app } = createApp();

app.listen(PORT, () => {
  // eslint-disable-next-line no-console
  console.log(`Garden Controller listening on port ${PORT}`);
});
