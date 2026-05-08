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

  test('api state includes desired/reported and queue depth for queued commands only', async () => {
    const app = build();
    await request(app).post('/api/commands').set('x-api-token', token).send({ channel: 1, action: 'on' });
    await request(app).get('/api/queue/next').set('x-api-token', token);

    const stateRes = await request(app).get('/api/state').set('x-api-token', token);
    expect(stateRes.status).toBe(200);
    expect(stateRes.body.queueDepth).toBe(0);
    expect(stateRes.body.desiredRelays).toBeDefined();
    expect(stateRes.body.reportedRelays).toBeDefined();
  });

  test('gui uses basic authentication', async () => {
    const app = build();
    expect((await request(app).get('/gui')).status).toBe(401);
    expect((await request(app).get('/gui').set('authorization', auth)).status).toBe(200);
  });

  test('openapi spec includes new endpoints', () => {
    const specPath = path.join(__dirname, '..', 'openapi.yaml');
    const spec = yaml.load(fs.readFileSync(specPath, 'utf8'));
    expect(spec.paths['/api/microcontroller/state']).toBeDefined();
    expect(spec.paths['/api/microcontroller/commands/{id}/ack']).toBeDefined();
    expect(spec.paths['/api/relays']).toBeDefined();
    expect(spec.paths['/api/state']).toBeDefined();
  });
});
