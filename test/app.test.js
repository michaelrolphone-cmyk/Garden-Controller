const request = require('supertest');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const { createApp } = require('../src/app');

describe('garden controller api', () => {
  const token = 'test-token';
  const auth = `Basic ${Buffer.from('admin:password').toString('base64')}`;

  function build() {
    return createApp({ apiKey: token, guiUsername: 'admin', guiPassword: 'password', enableWeatherRefresh: false }).app;
  }

  test('requires token for protected endpoints', async () => {
    const res = await request(build()).get('/api/relays');
    expect(res.status).toBe(401);
  });

  test('separates desired and reported relay state', async () => {
    const app = build();
    const queueRes = await request(app).post('/api/commands').set('x-api-token', token).send({ channel: 1, action: 'on' });
    expect(queueRes.status).toBe(201);

    const relaysRes = await request(app).get('/api/relays').set('x-api-token', token);
    expect(relaysRes.body.desiredRelays[0].state).toBe('on');
    expect(relaysRes.body.reportedRelays[0].state).toBe('off');
  });

  test('queue lifecycle delivered -> applied with ack endpoint', async () => {
    const app = build();
    const createRes = await request(app).post('/api/commands').set('x-api-token', token).send({ channel: 2, action: 'off' });
    const commandId = createRes.body.command.id;

    const nextRes = await request(app).get('/api/queue/next').set('x-api-token', token);
    expect(nextRes.status).toBe(200);
    expect(nextRes.body.command.id).toBe(commandId);
    expect(nextRes.body.command.status).toBe('delivered');

    const ackRes = await request(app)
      .post(`/api/microcontroller/commands/${commandId}/ack`)
      .set('x-api-token', token)
      .send({ status: 'applied' });
    expect(ackRes.status).toBe(200);
    expect(ackRes.body.command.status).toBe('applied');

    const stateRes = await request(app).get('/api/state').set('x-api-token', token);
    expect(stateRes.body.commandHistory).toHaveLength(1);
    expect(stateRes.body.commandHistory[0].id).toBe(commandId);
    expect(stateRes.body.queueDepth).toBe(0);
    expect(stateRes.body.deliveredDepth).toBe(0);
  });

  test('canonical microcontroller state endpoint persists telemetry', async () => {
    const app = build();
    const res = await request(app)
      .post('/api/microcontroller/state')
      .set('x-api-token', token)
      .send({
        deviceId: 'garden-relay-6',
        firmwareVersion: 'v12',
        clockValid: true,
        relays: [{ channel: 1, state: 'on' }],
        schedules: [{ channel: 1, zone: 'North', startTime: '06:00', durationSeconds: 300 }],
        currentRun: { channel: 1 },
        lastCommandId: 'abc'
      });

    expect(res.status).toBe(200);
    expect(res.body.telemetry.deviceId).toBe('garden-relay-6');
    expect(res.body.telemetry.lastSeenAt).toBeDefined();
  });



  test('stores firmware sensor payload via dedicated endpoint and exposes it in state', async () => {
    const app = build();
    const payload = {
      deviceId: 'garden-relay-6',
      firmwareVersion: 'v16-weather-sensor-baseline',
      targetLocation: { lat: 43.665288, lon: -116.259186, label: 'garden' },
      sensorData: [{ source: 'relay-hardware', type: 'wifi_rssi', value: -67, unit: 'dBm' }]
    };

    const postRes = await request(app).post('/api/microcontroller/sensors').set('x-api-token', token).send(payload);
    expect(postRes.status).toBe(201);

    const listRes = await request(app).get('/api/sensors?limit=10').set('x-api-token', token);
    expect(listRes.status).toBe(200);
    expect(listRes.body.readings).toHaveLength(1);
    expect(listRes.body.latest.deviceId).toBe('garden-relay-6');

    const stateRes = await request(app).get('/api/state').set('x-api-token', token);
    expect(stateRes.body.latestSensorData).toBeDefined();
    expect(stateRes.body.sensorReadingCount).toBe(1);
    expect(stateRes.body.weatherDatasets).toBeDefined();
  });

  test('canonical state telemetry retains targetLocation and sensorData', async () => {
    const app = build();
    const res = await request(app)
      .post('/api/microcontroller/state')
      .set('x-api-token', token)
      .send({
        deviceId: 'garden-relay-6',
        firmwareVersion: 'v16-weather-sensor-baseline',
        clockValid: true,
        relays: [{ channel: 1, state: 'on' }],
        schedules: [{ channel: 1, zone: 'North', startTime: '06:00', durationSeconds: 300 }],
        targetLocation: { lat: 43.665288, lon: -116.259186, label: 'garden' },
        sensorData: [{ source: 'relay-hardware', type: 'uptime', value: 921, unit: 's' }]
      });

    expect(res.status).toBe(200);
    expect(res.body.telemetry.targetLocation.label).toBe('garden');
    expect(res.body.telemetry.sensorData).toHaveLength(1);

    const latestRes = await request(app).get('/api/sensors/latest').set('x-api-token', token);
    expect(latestRes.body.latest.sourceEndpoint).toBe('/api/microcontroller/state');
  });

  test('canonical state telemetry retains device clock and connectivity metadata', async () => {
    const app = build();
    const res = await request(app)
      .post('/api/microcontroller/state')
      .set('x-api-token', token)
      .send({
        deviceId: 'garden-relay-6',
        firmwareVersion: 'v16-weather-sensor-baseline',
        clockValid: true,
        epoch: 1777901400,
        localTime: '6:30am',
        localDate: 'Monday, May 4th',
        homeWifiConnected: true,
        homeIp: '192.168.1.42',
        relays: [{ channel: 1, state: 'off' }],
        schedules: [{ channel: 1, zone: 'Zone 1', startTime: '06:00', durationSeconds: 600 }],
        currentRun: { active: false },
        lastCommandId: '1715144970000-a1b2c3'
      });

    expect(res.status).toBe(200);
    expect(res.body.telemetry.epoch).toBe(1777901400);
    expect(res.body.telemetry.localTime).toBe('6:30am');
    expect(res.body.telemetry.localDate).toBe('Monday, May 4th');
    expect(res.body.telemetry.homeWifiConnected).toBe(true);
    expect(res.body.telemetry.homeIp).toBe('192.168.1.42');
    expect(res.body.telemetry.currentRun).toEqual({ active: false });
    expect(res.body.telemetry.lastCommandId).toBe('1715144970000-a1b2c3');
  });

  test('refreshes and serves weather datasets', async () => {
    const app = build();
    const originalFetch = global.fetch;
    const point = { properties: { forecast: 'https://api.weather.gov/f', forecastHourly: 'https://api.weather.gov/h', forecastGridData: 'https://api.weather.gov/g' } };
    const ok = (data) => ({ ok: true, json: async () => data });
    global.fetch = jest.fn(async (url) => {
      if (String(url).includes('/points/')) return ok(point);
      if (String(url).endsWith('/f')) return ok({ periods: [] });
      if (String(url).endsWith('/h')) return ok({ periods: [] });
      if (String(url).endsWith('/g')) return ok({ grid: true });
      return { ok: false, status: 404, json: async () => ({}) };
    });

    const refreshRes = await request(app).post('/api/weather/refresh').set('x-api-token', token).send({});
    expect(refreshRes.status).toBe(200);
    expect(refreshRes.body.datasets.current.source).toBe('NWS');

    const datasetsRes = await request(app).get('/api/weather/datasets').set('x-api-token', token);
    expect(datasetsRes.status).toBe(200);
    expect(datasetsRes.body.datasets.updatedAt).toBeDefined();

    global.fetch = originalFetch;
  });

  test('api state includes desired/reported queue and telemetry metadata', async () => {
    const app = build();
    await request(app).post('/api/commands').set('x-api-token', token).send({ channel: 1, action: 'on' });
    await request(app).get('/api/queue/next').set('x-api-token', token);

    const stateRes = await request(app).get('/api/state').set('x-api-token', token);
    expect(stateRes.status).toBe(200);
    expect(stateRes.body.queueDepth).toBe(0);
    expect(stateRes.body.deliveredDepth).toBe(1);
    expect(stateRes.body.pendingPolls).toBe(0);
    expect(stateRes.body.desiredRelays).toBeDefined();
    expect(stateRes.body.reportedRelays).toBeDefined();
    expect(stateRes.body.deviceTelemetry).toBeNull();
  });

  test('api schedules queue schedule_update command with queued status', async () => {
    const app = build();
    const res = await request(app)
      .post('/api/schedules')
      .set('x-api-token', token)
      .send({ schedules: [{ channel: 3, zone: 'Beds', startTime: '07:00', durationSeconds: 180 }] });
    expect(res.status).toBe(201);
    expect(res.body.command.status).toBe('queued');
  });


  test('POST /api/schedules accepts multiple entries for the same zone', async () => {
    const app = build();
    const res = await request(app)
      .post('/api/schedules')
      .set('x-api-token', token)
      .send({ schedules: [
        { channel: 1, zone: 'Zone 1', enabled: true, startTime: '06:00', durationSeconds: 600 },
        { channel: 1, zone: 'Zone 1', enabled: true, startTime: '15:30', durationSeconds: 420 }
      ] });
    expect(res.status).toBe(201);
    expect(res.body.schedules).toHaveLength(2);
    expect(res.body.command.type).toBe('schedule_update');
    expect(res.body.command.schedules).toHaveLength(2);
  });


  test('DELETE /api/schedules/:id removes a single schedule entry and queues schedule update', async () => {
    const app = build();
    await request(app)
      .post('/api/schedules')
      .set('x-api-token', token)
      .send({ schedules: [
        { id: 11, channel: 1, zone: 'Zone 1', enabled: true, startTime: '06:00', durationSeconds: 600 },
        { id: 12, channel: 1, zone: 'Zone 1', enabled: true, startTime: '15:30', durationSeconds: 420 }
      ] });

    const deleteRes = await request(app).delete('/api/schedules/11').set('x-api-token', token);
    expect(deleteRes.status).toBe(200);
    expect(deleteRes.body.schedules).toHaveLength(1);
    expect(deleteRes.body.schedules[0].id).toBe(12);
    expect(deleteRes.body.command.type).toBe('schedule_update');
    expect(deleteRes.body.command.schedules).toHaveLength(1);
  });

  test('POST /api/schedules accepts empty list to clear all schedules', async () => {
    const app = build();
    const res = await request(app).post('/api/schedules').set('x-api-token', token).send({ schedules: [] });
    expect(res.status).toBe(201);
    expect(res.body.schedules).toEqual([]);
    expect(res.body.command.schedules).toEqual([]);
  });


  test('gui timeline groups multiple daily runs for same zone into one row', async () => {
    const app = build();
    await request(app)
      .post('/api/schedules')
      .set('x-api-token', token)
      .send({ schedules: [
        { channel: 1, zone: 'Zone 1', enabled: true, startTime: '06:00', durationSeconds: 600 },
        { channel: 1, zone: 'Zone 1', enabled: true, startTime: '15:30', durationSeconds: 420 },
        { channel: 2, zone: 'Zone 2', enabled: true, startTime: '07:00', durationSeconds: 300 }
      ] });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);

    const zone1RowMatches = guiRes.text.match(/<div class="timeline-row"><span class="timeline-zone">Zone 1<\/span>/g) || [];
    expect(zone1RowMatches).toHaveLength(1);

    const zone1Row = guiRes.text.match(/<div class="timeline-row"><span class="timeline-zone">Zone 1<\/span><div class="timeline-track">([\s\S]*?)<\/div><\/div>/);
    expect(zone1Row).not.toBeNull();
    const zone1Blocks = (zone1Row[1].match(/class="timeline-block"/g) || []).length;
    expect(zone1Blocks).toBe(2);
  });

  test('gui does not prefill phantom 6:00 zone 1 schedule when no schedules are configured', async () => {
    const app = build();
    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('No schedules configured.');
    expect(guiRes.text).not.toContain('name="schedule[0][startTime]" type="time" value="06:00"');

    const postRes = await request(app).post('/gui/schedules').set('authorization', auth).send({});
    expect(postRes.status).toBe(303);

    const stateRes = await request(app).get('/api/state').set('x-api-token', token);
    expect(stateRes.body.schedules).toEqual([]);
  });

  test('POST /api/schedules rejects oversized schedule list', async () => {
    const app = build();
    const schedules = Array.from({ length: 65 }, (_, index) => ({ channel: (index % 5) + 1, zone: `Zone ${(index % 5) + 1}`, startTime: '06:00', durationSeconds: 300 }));
    const res = await request(app).post('/api/schedules').set('x-api-token', token).send({ schedules });
    expect(res.status).toBe(400);
    expect(res.body.error).toBe('Too many schedule entries');
  });

  test('POST /api/schedules preserves enabled=false', async () => {
    const app = build();
    const res = await request(app).post('/api/schedules').set('x-api-token', token).send({ schedules: [{ channel: 2, zone: 'Zone 2', enabled: false, startTime: '09:00', durationSeconds: 600 }] });
    expect(res.status).toBe(201);
    expect(res.body.schedules[0].enabled).toBe(false);
    expect(res.body.command.schedules[0].enabled).toBe(false);
  });

  test('POST /api/microcontroller/state stores repeated-channel schedules', async () => {
    const app = build();
    const res = await request(app).post('/api/microcontroller/state').set('x-api-token', token).send({
      deviceId: 'garden-relay-6', firmwareVersion: 'v23', relays: [{ channel: 1, state: 'off' }],
      schedules: [
        { id: 11, channel: 1, zone: 'Zone 1', enabled: true, startTime: '06:00', durationSeconds: 600 },
        { id: 12, channel: 1, zone: 'Zone 1', enabled: false, startTime: '15:30', durationSeconds: 420 }
      ]
    });
    expect(res.status).toBe(200);
    const stateRes = await request(app).get('/api/state').set('x-api-token', token);
    expect(stateRes.body.schedules).toHaveLength(2);
    expect(stateRes.body.schedules[1].enabled).toBe(false);
    expect(stateRes.body.schedules[1].id).toBe(12);
  });

  test('gui renders multiple schedule entries for same channel without collapsing', async () => {
    const app = build();
    await request(app).post('/api/schedules').set('x-api-token', token).send({ schedules: [
      { channel: 1, zone: 'Zone 1', enabled: true, startTime: '15:30', durationSeconds: 420 },
      { channel: 1, zone: 'Zone 1', enabled: true, startTime: '06:00', durationSeconds: 600 }
    ]});
    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('15:30 · 7 min');
    expect(guiRes.text).toContain('06:00 · 10 min');
    expect(guiRes.text).toContain('name="schedule[0][enabled]"');
    expect(guiRes.text).toContain('name="schedule[1][enabled]"');
    expect(guiRes.text).toContain('<th>Delete</th>');
    expect(guiRes.text).toContain('formaction="/gui/schedules/0/delete"');
  });
  test('gui schedule delete endpoint removes one row and queues replacement schedule update', async () => {
    const app = build();
    await request(app).post('/api/schedules').set('x-api-token', token).send({ schedules: [
      { id: 11, channel: 1, zone: 'Zone 1', enabled: true, startTime: '06:00', durationSeconds: 600 },
      { id: 12, channel: 1, zone: 'Zone 1', enabled: true, startTime: '15:30', durationSeconds: 420 }
    ]});

    const deleteRes = await request(app).post('/gui/schedules/11/delete').set('authorization', auth);
    expect(deleteRes.status).toBe(303);

    const stateRes = await request(app).get('/api/state').set('x-api-token', token);
    expect(stateRes.body.schedules).toHaveLength(1);
    expect(stateRes.body.schedules[0].id).toBe(12);

    await request(app).get('/api/queue/next').set('x-api-token', token);
    const commandRes = await request(app).get('/api/queue/next').set('x-api-token', token);
    expect(commandRes.status).toBe(200);
    expect(commandRes.body.command.type).toBe('schedule_update');
    expect(commandRes.body.command.schedules).toHaveLength(1);
    expect(commandRes.body.command.schedules[0].id).toBe(12);
  });
  test('queue next supports long-poll and wakes when new command is queued', async () => {
    const app = build();
    const pollPromise = request(app).get('/api/queue/next?wait=1').set('x-api-token', token);
    await new Promise((resolve) => setTimeout(resolve, 50));
    await request(app).post('/api/commands').set('x-api-token', token).send({ channel: 4, action: 'on' });
    const pollRes = await pollPromise;
    expect(pollRes.status).toBe(200);
    expect(pollRes.body.command.channel).toBe(4);
    expect(pollRes.body.command.status).toBe('delivered');
  });

  test('gui uses basic authentication', async () => {
    const app = build();
    expect((await request(app).get('/gui')).status).toBe(401);
    expect((await request(app).get('/gui').set('authorization', auth)).status).toBe(200);
  });


  test('gui relay controls use explicit on/off and show desired/reported mismatch', async () => {
    const app = build();

    await request(app)
      .post('/api/commands')
      .set('x-api-token', token)
      .send({ channel: 1, action: 'on' });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('Zones (scheduled irrigation)');
    expect(guiRes.text).toContain('Desired <strong>ON</strong>');
    expect(guiRes.text).toContain('Reported <strong>OFF</strong>');
    expect(guiRes.text).toContain('status-pill status-mismatch');
    expect(guiRes.text).toContain('Garden Zone Map');
    expect(guiRes.text).not.toContain('<meta http-equiv="refresh" content="1" />');
    expect(guiRes.text).toContain("setInterval(refreshState, 1000);");
    expect(guiRes.text).toContain('Castle Hills Garden Manager');
    expect(guiRes.text).toContain('grid-template-columns: 1fr 2fr');
    expect(guiRes.text).toContain('id="zone-2"');
    expect(guiRes.text).toContain('zone-theme-1');
    expect(guiRes.text).toContain('id="zone-4a"');
    expect(guiRes.text).toContain('id="zone-4b"');
    expect(guiRes.text).toContain("{1:'zone-1',2:'zone-2',3:'zone-3',4:'zone-4a zone-4b',5:'zone-5'}");
    expect(guiRes.text).not.toContain('Zone 6');
    expect(guiRes.text).toContain('.linework { stroke: rgba(29, 78, 216, 0.35); fill: none;');
    expect(guiRes.text).toContain('data-active="false"');
    expect(guiRes.text).toContain('/gui/relays/1/on');
    expect(guiRes.text).toContain('/gui/relays/1/off');
    expect(guiRes.text).toContain('Run Spigots');
    expect(guiRes.text).toContain('Stop Spigots');
    expect(guiRes.text).toContain('timeline-track');
    expect(guiRes.text).toContain('Raw schedule list');
    expect(guiRes.text.indexOf('<h2>Schedules</h2>')).toBeGreaterThan(-1);
    expect(guiRes.text.indexOf('Environmental Monitoring (43.665288, -116.259186)')).toBeGreaterThan(-1);
    expect(guiRes.text.indexOf('<h2>Schedules</h2>')).toBeLessThan(guiRes.text.indexOf('Environmental Monitoring (43.665288, -116.259186)'));
    expect(guiRes.text).toContain('Environmental Monitoring (43.665288, -116.259186)');
    expect(guiRes.text).toContain('overlay=radar');
    expect(guiRes.text).toContain('overlay=satellite');
    expect(guiRes.text).toContain('USGS National Map (3DEP)');
    expect(guiRes.text).toContain('Microcontroller Weather Sensors');
    expect(guiRes.text).toContain('No microcontroller weather sensor data reported yet.');
    expect(guiRes.text).toContain('Device Telemetry');
    expect(guiRes.text).toContain('No device telemetry reported yet.');
    expect(guiRes.text).not.toContain('/gui/relays/1/toggle');
  });





  test('gui renders microcontroller weather sensor data table', async () => {
    const app = build();
    await request(app)
      .post('/api/microcontroller/sensors')
      .set('x-api-token', token)
      .send({
        deviceId: 'garden-relay-6',
        firmwareVersion: 'v16-weather-sensor-baseline',
        targetLocation: { lat: 43.665288, lon: -116.259186, label: 'garden' },
        sensorData: [
          { source: 'relay-hardware', type: 'temperature_c', value: 21.7, unit: 'C' },
          { source: 'relay-hardware', type: 'humidity_pct', value: 54, unit: '%' }
        ]
      });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('id="sensor-table"');
    expect(guiRes.text).toContain('temperature_c');
    expect(guiRes.text).toContain('humidity_pct');
    expect(guiRes.text).toContain('relay-hardware');

    const stateRes = await request(app).get('/gui/state').set('authorization', auth);
    expect(stateRes.status).toBe(200);
    expect(stateRes.body.latestSensorData.sensorData).toHaveLength(2);
  });

  test('gui renders telemetry table with last seen relative time', async () => {
    const app = build();
    await request(app)
      .post('/api/microcontroller/state')
      .set('x-api-token', token)
      .send({
        deviceId: 'garden-relay-6',
        firmwareVersion: 'v17',
        clockValid: true,
        relays: [{ channel: 1, state: 'on' }],
        schedules: [{ channel: 1, zone: 'North', startTime: '06:00', durationSeconds: 300 }],
        currentRun: { channel: 1, remainingSeconds: 120 },
        lastCommandId: 'cmd-123',
        targetLocation: { lat: 43.665288, lon: -116.259186, label: 'garden' },
        sensorData: [{ source: 'relay-hardware', type: 'temperature_c', value: 20.1, unit: 'C' }]
      });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('id="telemetry-table"');
    expect(guiRes.text).toContain('garden-relay-6');
    expect(guiRes.text).toContain('v17');
    expect(guiRes.text).toContain('cmd-123');
    expect(guiRes.text).toContain('Last Seen');
    expect(guiRes.text).toMatch(/\d+s ago/);

    const stateRes = await request(app).get('/gui/state').set('authorization', auth);
    expect(stateRes.status).toBe(200);
    expect(stateRes.body.deviceTelemetry.deviceId).toBe('garden-relay-6');
    expect(stateRes.body.deviceTelemetry.lastSeenAt).toBeDefined();
  });
  test('gui renders schedule as a visual timeline block', async () => {
    const app = build();
    await request(app)
      .post('/api/microcontroller/schedules')
      .set('x-api-token', token)
      .send({ schedules: [{ channel: 3, zone: 'Vegetable Garden', startTime: '13:00', durationSeconds: 1800 }] });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('Vegetable Garden');
    expect(guiRes.text).toContain('timeline-block');
    expect(guiRes.text).toContain('13:00 · 30 min');
  });

  test('gui renders multiple daily schedules for a zone on the same timeline row', async () => {
    const app = build();
    await request(app)
      .post('/api/microcontroller/schedules')
      .set('x-api-token', token)
      .send({
        schedules: [
          { channel: 2, zone: 'Herbs', startTime: '06:00', durationSeconds: 600 },
          { channel: 2, zone: 'Herbs', startTime: '18:00', durationSeconds: 300 }
        ]
      });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('06:00 · 10 min');
    expect(guiRes.text).toContain('18:00 · 5 min');
    expect(guiRes.text.match(/<span class="timeline-zone">Herbs<\/span>/g)).toHaveLength(1);
    expect(guiRes.text).toMatch(/<span class="timeline-zone">Herbs<\/span><div class="timeline-track">.*06:00 · 10 min.*18:00 · 5 min.*<\/div><\/div>/s);
  });

  test('gui schedule form accepts minutes and stores seconds', async () => {
    const app = build();
    const res = await request(app)
      .post('/gui/schedules')
      .set('authorization', auth)
      .send({ channel: '2', zone: 'Herbs', startTime: '07:30', durationMinutes: '12' });

    expect(res.status).toBe(303);

    const stateRes = await request(app).get('/api/state').set('x-api-token', token);
    expect(stateRes.body.schedules[0].durationSeconds).toBe(720);

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.text).toContain('for 12 min');
    expect(guiRes.text).toContain('Duration Minutes');
  });

  test('gui schedule form preserves schedule ids when editing existing entries', async () => {
    const app = build();
    await request(app)
      .post('/api/microcontroller/schedules')
      .set('x-api-token', token)
      .send({ schedules: [{ id: 11, channel: 2, zone: 'Herbs', enabled: true, startTime: '07:30', durationSeconds: 720 }] });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('name="schedule[0][id]" type="hidden" value="11"');

    const postRes = await request(app)
      .post('/gui/schedules')
      .set('authorization', auth)
      .send({
        'schedule[0][id]': '11',
        'schedule[0][enabled]': 'on',
        'schedule[0][channel]': '2',
        'schedule[0][zone]': 'Herbs',
        'schedule[0][startTime]': '08:00',
        'schedule[0][durationMinutes]': '10'
      });
    expect(postRes.status).toBe(303);

    const stateRes = await request(app).get('/api/state').set('x-api-token', token);
    expect(stateRes.body.schedules[0].id).toBe(11);
    expect(stateRes.body.schedules[0].startTime).toBe('08:00');

    const queuedRes = await request(app).get('/api/queue/next').set('x-api-token', token);
    expect(queuedRes.status).toBe(200);
    expect(queuedRes.body.command.schedules[0].id).toBe(11);
  });

  test('gui schedule form supports adding zone time slots and queues full schedule payload', async () => {
    const app = build();
    await request(app)
      .post('/api/microcontroller/schedules')
      .set('x-api-token', token)
      .send({ schedules: [{ id: 9, channel: 1, zone: 'Zone 1', enabled: true, startTime: '06:00', durationSeconds: 600 }] });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('id="schedule-add-row"');
    expect(guiRes.text).toContain('submits the complete schedule list');

    const postRes = await request(app)
      .post('/gui/schedules')
      .set('authorization', auth)
      .send({
        'schedule[0][id]': '9',
        'schedule[0][enabled]': 'on',
        'schedule[0][channel]': '1',
        'schedule[0][zone]': 'Zone 1',
        'schedule[0][startTime]': '06:00',
        'schedule[0][durationMinutes]': '10',
        'schedule[1][id]': '10',
        'schedule[1][enabled]': 'on',
        'schedule[1][channel]': '1',
        'schedule[1][zone]': 'Zone 1',
        'schedule[1][startTime]': '15:30',
        'schedule[1][durationMinutes]': '7'
      });
    expect(postRes.status).toBe(303);

    const queuedRes = await request(app).get('/api/queue/next').set('x-api-token', token);
    expect(queuedRes.status).toBe(200);
    expect(queuedRes.body.command.type).toBe('schedule_update');
    expect(queuedRes.body.command.schedules).toHaveLength(2);
    expect(queuedRes.body.command.schedules[0].id).toBe(9);
    expect(queuedRes.body.command.schedules[1].startTime).toBe('15:30');
  });

  test('gui map marks active zones from reported relay state', async () => {
    const app = build();

    await request(app)
      .post('/api/microcontroller/relays/state')
      .set('x-api-token', token)
      .send({ relays: [{ channel: 2, state: 'on' }] });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('id="zone-2" class="zone zone-theme-2 zone-active" data-active="true"');
    expect(guiRes.text).toContain('id="zone-1" class="zone zone-theme-1 " data-active="false"');
    expect(guiRes.text).toContain('filter: saturate(1.4) brightness(1.15) drop-shadow(0 0 18px rgba(var(--zone-color-rgb, 62, 184, 255), 0.72));');
  });


  test('gui map maps reported channel 4 to both zone 4 polygons', async () => {
    const app = build();

    await request(app)
      .post('/api/microcontroller/relays/state')
      .set('x-api-token', token)
      .send({ relays: [{ channel: 4, state: 'on' }] });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('id="zone-4a" class="zone zone-theme-4 zone-active" data-active="true"');
    expect(guiRes.text).toContain('id="zone-4b" class="zone zone-theme-4 zone-active" data-active="true"');
    expect(guiRes.text).toContain("flatMap((relay) => zoneShapeIdsForChannel(relay.channel))");
  });

  test('gui relay action endpoint only allows explicit on/off actions', async () => {
    const app = build();

    const onRes = await request(app).post('/gui/relays/2/on').set('authorization', auth);
    expect(onRes.status).toBe(303);

    const relaysRes = await request(app).get('/api/relays').set('x-api-token', token);
    expect(relaysRes.body.desiredRelays[1].state).toBe('on');
    expect(relaysRes.body.reportedRelays[1].state).toBe('off');

    const invalidRes = await request(app).post('/gui/relays/2/toggle').set('authorization', auth);
    expect(invalidRes.status).toBe(400);
  });
  test('gui state endpoint returns only refreshable hardware state data', async () => {
    const app = build();
    await request(app)
      .post('/api/microcontroller/schedules')
      .set('x-api-token', token)
      .send({ schedules: [{ channel: 2, zone: 'Front Beds', startTime: '08:00', durationSeconds: 600 }] });
    await request(app)
      .post('/api/microcontroller/relays/state')
      .set('x-api-token', token)
      .send({ relays: [{ channel: 2, state: 'on' }] });

    const stateRes = await request(app).get('/gui/state').set('authorization', auth);
    expect(stateRes.status).toBe(200);
    expect(stateRes.body.desiredRelays).toBeDefined();
    expect(stateRes.body.reportedRelays).toBeDefined();
    expect(stateRes.body.schedules).toHaveLength(1);
    expect(stateRes.body.serverTime).toBeDefined();
    expect(stateRes.body.radarEmbedUrl).toBeUndefined();
  });

  test('gui refresh script repopulates schedule edit rows from refreshed zone schedules', async () => {
    const app = build();
    await request(app)
      .post('/api/microcontroller/schedules')
      .set('x-api-token', token)
      .send({ schedules: [{ id: 22, channel: 2, zone: 'Front Beds', startTime: '08:00', durationSeconds: 600 }] });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain("const scheduleFormBody = document.getElementById('schedule-form-body');");
    expect(guiRes.text).toContain('scheduleFormBody.innerHTML = parsedSchedules.map((schedule, index) =>');
    expect(guiRes.text).toContain('name="schedule[\' + index + \'][startTime]" type="time" value="\' + startTime + \'"');
  });
  

  test('POST /api/schedules rejects channel 6', async () => {
    const app = build();
    const res = await request(app).post('/api/schedules').set('x-api-token', token).send({ schedules: [{ channel: 6, zone: 'Bad', startTime: '06:00', durationSeconds: 60 }] });
    expect(res.status).toBe(400);
  });

  
  test('zone on commands default to 15-minute timed runs', async () => {
    const app = build();
    const res = await request(app).post('/api/commands').set('x-api-token', token).send({ channel: 1, action: 'on' });
    expect(res.status).toBe(201);
    expect(res.body.command.durationSeconds).toBe(900);
  });
test('POST /api/commands accepts channel 6 with durationSeconds', async () => {
    const app = build();
    const res = await request(app).post('/api/commands').set('x-api-token', token).send({ channel: 6, action: 'on', durationSeconds: 1800 });
    expect(res.status).toBe(201);
    expect(res.body.command.channel).toBe(6);
    expect(res.body.command.durationSeconds).toBe(1800);
  });

  test('spigot run/stop endpoints queue channel 6 commands', async () => {
    const app = build();
    const runRes = await request(app).post('/api/spigots/run').set('x-api-token', token).send({ durationSeconds: 120 });
    expect(runRes.status).toBe(201);
    expect(runRes.body.command.channel).toBe(6);
    expect(runRes.body.command.action).toBe('on');

    const stopRes = await request(app).post('/api/spigots/stop').set('x-api-token', token).send({});
    expect(stopRes.status).toBe(201);
    expect(stopRes.body.command.channel).toBe(6);
    expect(stopRes.body.command.action).toBe('off');
  });
test('openapi spec includes new endpoints', () => {
    const specPath = path.join(__dirname, '..', 'openapi.yaml');
    const spec = yaml.load(fs.readFileSync(specPath, 'utf8'));
    expect(spec.paths['/api/microcontroller/state']).toBeDefined();
    expect(spec.paths['/api/microcontroller/commands/{id}/ack']).toBeDefined();
    expect(spec.paths['/api/relays']).toBeDefined();
    expect(spec.paths['/api/state']).toBeDefined();
    expect(spec.paths['/api/queue/next'].get.parameters[0].name).toBe('wait');
    expect(spec.paths['/gui/relays/{channel}/on']).toBeDefined();
    expect(spec.paths['/gui/relays/{channel}/off']).toBeDefined();
    expect(spec.paths['/gui/state']).toBeDefined();
    expect(spec.paths['/api/spigots/run']).toBeDefined();
    expect(spec.paths['/api/spigots/stop']).toBeDefined();
    expect(spec.paths['/gui/schedules/{id}/delete']).toBeDefined();
  });
});
