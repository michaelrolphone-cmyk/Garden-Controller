const request = require('supertest');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const { createApp } = require('../src/app');

describe('garden controller api', () => {
  const token = 'test-token';
  const auth = `Basic ${Buffer.from('admin:password').toString('base64')}`;

  function build() {
    return createApp({ apiKey: token, guiUsername: 'admin', guiPassword: 'password' }).app;
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
    expect(guiRes.text).toContain('Relay states (desired vs reported)');
    expect(guiRes.text).toContain('Desired <strong>ON</strong>');
    expect(guiRes.text).toContain('Reported <strong>OFF</strong>');
    expect(guiRes.text).toContain('status-pill status-mismatch');
    expect(guiRes.text).toContain('Garden Zone Map');
    expect(guiRes.text).toContain('<meta http-equiv="refresh" content="1" />');
    expect(guiRes.text).toContain('Castle Hills Garden Manager');
    expect(guiRes.text).toContain('grid-template-columns: 1fr 2fr');
    expect(guiRes.text).toContain('id="zone-6"');
    expect(guiRes.text).toContain('.linework { stroke: rgba(29, 78, 216, 0.35); fill: none;');
    expect(guiRes.text).toContain('data-active="false"');
    expect(guiRes.text).toContain('/gui/relays/1/on');
    expect(guiRes.text).toContain('/gui/relays/1/off');
    expect(guiRes.text).toContain('timeline-track');
    expect(guiRes.text).toContain('Raw schedule list');
    expect(guiRes.text).toContain('Environmental Monitoring (43.665288, -116.259186)');
    expect(guiRes.text).toContain('overlay=radar');
    expect(guiRes.text).toContain('overlay=satellite');
    expect(guiRes.text).toContain('USGS National Map (3DEP)');
    expect(guiRes.text).not.toContain('/gui/relays/1/toggle');
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
    expect(guiRes.text).toContain('13:00 · 1800s');
  });
  test('gui map marks active zones from reported relay state', async () => {
    const app = build();

    await request(app)
      .post('/api/microcontroller/relays/state')
      .set('x-api-token', token)
      .send({ relays: [{ channel: 2, state: 'on' }] });

    const guiRes = await request(app).get('/gui').set('authorization', auth);
    expect(guiRes.status).toBe(200);
    expect(guiRes.text).toContain('id="zone-2" class="zone zone-active" data-active="true"');
    expect(guiRes.text).toContain('id="zone-1" class="zone " data-active="false"');
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
  });
});
