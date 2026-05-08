const request = require('supertest');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const { createApp } = require('../src/app');

describe('garden controller api', () => {
  const auth = `Basic ${Buffer.from('admin:password').toString('base64')}`;

  function build() {
    return createApp({ apiKey: token, guiUsername: 'admin', guiPassword: 'password' }).app;
  }

  test('health check', async () => {
    const res = await request(build()).get('/health');
    expect(res.status).toBe(200);
    expect(res.body.ok).toBe(true);
  });

  test('allows api calls without api token header', async () => {
    const res = await request(build()).get('/api/relays');
    expect(res.status).toBe(200);
  });

  test('publishes full controller state', async () => {
    const app = createApp({
      apiKey: token,
      guiUsername: 'admin',
      guiPassword: 'password',
      state: {
        relayState: [{ channel: 1, state: 'on' }, { channel: 2, state: 'off' }],
        queue: [{ id: '1', channel: 1, action: 'toggle', requestedBy: 'gui', createdAt: '2026-01-01T00:00:00.000Z' }],
        news: [],
        schedules: [
          { channel: 1, zone: 'North Bed', startTime: '06:00', durationSeconds: 900 },
          { channel: 2, zone: 'South Bed', startTime: '06:15', durationSeconds: 600 }
        ]
      }
    }).app;

    const res = await request(app).get('/api/state');
    expect(res.status).toBe(200);
    expect(res.body.relays).toEqual([{ channel: 1, state: 'on' }, { channel: 2, state: 'off' }]);
    expect(res.body.queueDepth).toBe(1);
    expect(res.body.schedules).toEqual([
      { channel: 1, zone: 'North Bed', startTime: '06:00', durationSeconds: 900 },
      { channel: 2, zone: 'South Bed', startTime: '06:15', durationSeconds: 600 }
    ]);
    expect(new Date(res.body.serverTime).toString()).not.toBe('Invalid Date');
  });

  test('queues command and microcontroller pulls it', async () => {
    const app = build();
    const queueRes = await request(app)
      .post('/api/commands')
            .send({ channel: 1, action: 'on' });
    expect(queueRes.status).toBe(201);

    const nextRes = await request(app)
      .get('/api/queue/next')
      ;
    expect(nextRes.status).toBe(200);
    expect(nextRes.body.command.channel).toBe(1);
    expect(nextRes.body.relay.state).toBe('on');

    const empty = await request(app)
      .get('/api/queue/next')
      ;
    expect(empty.status).toBe(204);
  });

  test('microcontroller can publish current relay states', async () => {
    const app = build();

    const publishRes = await request(app)
      .post('/api/microcontroller/relays/state')
            .send({
        relays: [
          { channel: 1, state: 'on' },
          { channel: 3, state: 'on' },
          { channel: 6, state: 'off' }
        ]
      });

    expect(publishRes.status).toBe(200);
    expect(publishRes.body.relays).toEqual([
      { channel: 1, state: 'on' },
      { channel: 2, state: 'off' },
      { channel: 3, state: 'on' },
      { channel: 4, state: 'off' },
      { channel: 5, state: 'off' },
      { channel: 6, state: 'off' }
    ]);
  });

  test('microcontroller relay state publish validates payload', async () => {
    const app = build();

    const publishRes = await request(app)
      .post('/api/microcontroller/relays/state')
            .send({ relays: [{ channel: 0, state: 'on' }] });

    expect(publishRes.status).toBe(400);
  });

  test('microcontroller can publish relay schedules', async () => {
    const app = build();

    const publishRes = await request(app)
      .post('/api/microcontroller/schedules')
            .send({
        schedules: [
          { channel: 1, zone: 'Herbs', startTime: '06:00', durationSeconds: 900 },
          { channel: 2, zone: 'Tomatoes', startTime: '06:20', durationSeconds: 600 }
        ]
      });

    expect(publishRes.status).toBe(200);
    expect(publishRes.body.schedules).toEqual([
      { channel: 1, zone: 'Herbs', startTime: '06:00', durationSeconds: 900 },
      { channel: 2, zone: 'Tomatoes', startTime: '06:20', durationSeconds: 600 }
    ]);

    const stateRes = await request(app).get('/api/state');
    expect(stateRes.status).toBe(200);
    expect(stateRes.body.schedules).toEqual([
      { channel: 1, zone: 'Herbs', startTime: '06:00', durationSeconds: 900 },
      { channel: 2, zone: 'Tomatoes', startTime: '06:20', durationSeconds: 600 }
    ]);
  });

  test('microcontroller schedule publish validates payload', async () => {
    const app = build();

    const publishRes = await request(app)
      .post('/api/microcontroller/schedules')
            .send({ schedules: [{ channel: 2, startTime: '', durationSeconds: 0 }] });

    expect(publishRes.status).toBe(400);
  });

  test('api can set schedules for microcontroller queue', async () => {
    const app = build();
    const res = await request(app)
      .post('/api/schedules')
            .send({
        schedules: [{ channel: 4, zone: 'Greenhouse', startTime: '07:00', durationSeconds: 300 }],
        requestedBy: 'operator'
      });
    expect(res.status).toBe(201);
    expect(res.body.schedules).toEqual([
      { channel: 4, zone: 'Greenhouse', startTime: '07:00', durationSeconds: 300 }
    ]);

    const nextRes = await request(app).get('/api/queue/next');
    expect(nextRes.status).toBe(200);
    expect(nextRes.body.command.type).toBe('schedule_update');
    expect(nextRes.body.command.schedules).toEqual([
      { channel: 4, zone: 'Greenhouse', startTime: '07:00', durationSeconds: 300 }
    ]);
  });

  test('validates command payload', async () => {
    const res = await request(build())
      .post('/api/commands')
            .send({ channel: 9, action: 'on' });
    expect(res.status).toBe(400);
  });

  test('news feed create and read', async () => {
    const app = build();
    const createRes = await request(app)
      .post('/api/news')
            .send({ title: 'Test', body: 'Body' });
    expect(createRes.status).toBe(201);

    const list = await request(app)
      .get('/api/news')
      ;
    expect(list.status).toBe(200);
    expect(list.body.news).toHaveLength(1);
  });

  test('gui uses basic authentication', async () => {
    const app = build();
    const unauthorized = await request(app).get('/gui');
    expect(unauthorized.status).toBe(401);

    const authorized = await request(app).get('/gui').set('authorization', auth);
    expect(authorized.status).toBe(200);
    expect(authorized.text).toContain('ESP32-S3-Relay-6CH Controller');
    expect(authorized.text).toContain('Current server time (UTC)');
    expect(authorized.text).toContain('Toggle');
    expect(authorized.text).toContain('Update schedules');
  });

  test('gui toggle control queues a command', async () => {
    const app = createApp({ apiKey: token, guiUsername: 'admin', guiPassword: 'password' });

    const res = await request(app.app)
      .post('/gui/relays/1/toggle')
      .set('authorization', auth)
      .redirects(0);

    expect(res.status).toBe(303);
    expect(app.state.queue).toHaveLength(1);
    expect(app.state.queue[0]).toMatchObject({ channel: 1, action: 'toggle', requestedBy: 'gui-web' });
  });

  test('gui can save schedule and queue schedule update', async () => {
    const app = createApp({ apiKey: token, guiUsername: 'admin', guiPassword: 'password' });

    const res = await request(app.app)
      .post('/gui/schedules')
      .set('authorization', auth)
      .type('form')
      .send({ channel: '2', zone: 'Patio', startTime: '06:45', durationSeconds: '480' })
      .redirects(0);

    expect(res.status).toBe(303);
    expect(app.state.schedules).toEqual([{ channel: 2, zone: 'Patio', startTime: '06:45', durationSeconds: 480 }]);
    expect(app.state.queue[0].type).toBe('schedule_update');
  });

  test('gui auth supports colons in password from env-style credentials', async () => {
    const app = createApp({ apiKey: token, guiUsername: 'admin', guiPassword: 'pa:ss:word' }).app;
    const colonPasswordAuth = `Basic ${Buffer.from('admin:pa:ss:word').toString('base64')}`;

    const authorized = await request(app).get('/gui').set('authorization', colonPasswordAuth);
    expect(authorized.status).toBe(200);
  });

  test('reads GUI credentials from process.env when config is not provided', async () => {
    process.env.GUI_USERNAME = 'test';
    process.env.GUI_PASSWORD = 'test';

    const app = createApp({ apiKey: token }).app;
    const envAuth = `Basic ${Buffer.from('test:test').toString('base64')}`;
    const authorized = await request(app).get('/gui').set('authorization', envAuth);

    expect(authorized.status).toBe(200);

    delete process.env.GUI_USERNAME;
    delete process.env.GUI_PASSWORD;
  });


  test('reads quoted GUI credentials from process.env', async () => {
    process.env.GUI_USERNAME = '"quoted-user"';
    process.env.GUI_PASSWORD = "'quoted-pass'";

    const app = createApp({ apiKey: token }).app;
    const envAuth = `Basic ${Buffer.from('quoted-user:quoted-pass').toString('base64')}`;
    const authorized = await request(app).get('/gui').set('authorization', envAuth);

    expect(authorized.status).toBe(200);

    delete process.env.GUI_USERNAME;
    delete process.env.GUI_PASSWORD;
  });


  test('reads quoted API token from process.env', async () => {
    process.env.API_KEY = '"quoted-token"';

    const app = createApp({ guiUsername: 'admin', guiPassword: 'password' }).app;
    const authorized = await request(app).get('/api/state').set('x-api-token', 'quoted-token');

    expect(authorized.status).toBe(200);

    delete process.env.API_KEY;
  });

  test('reads API token with surrounding whitespace from process.env', async () => {
    process.env.API_KEY = '  spaced-token  ';

    const app = createApp({ guiUsername: 'admin', guiPassword: 'password' }).app;
    const authorized = await request(app).get('/api/state').set('x-api-token', 'spaced-token');

    expect(authorized.status).toBe(200);

    delete process.env.API_KEY;
  });

  test('accepts lowercase basic auth scheme', async () => {
    const app = build();
    const lowercaseSchemeAuth = `basic ${Buffer.from('admin:password').toString('base64')}`;

    const authorized = await request(app).get('/gui').set('authorization', lowercaseSchemeAuth);
    expect(authorized.status).toBe(200);
  });

  test('openapi spec includes implemented endpoints', () => {
    const specPath = path.join(__dirname, '..', 'openapi.yaml');
    const spec = yaml.load(fs.readFileSync(specPath, 'utf8'));
    expect(spec.paths['/api/relays']).toBeDefined();
    expect(spec.paths['/api/state']).toBeDefined();
    expect(spec.paths['/api/commands']).toBeDefined();
    expect(spec.paths['/api/schedules']).toBeDefined();
    expect(spec.paths['/api/queue/next']).toBeDefined();
    expect(spec.paths['/api/microcontroller/relays/state']).toBeDefined();
    expect(spec.paths['/api/microcontroller/schedules']).toBeDefined();
    expect(spec.paths['/api/news']).toBeDefined();
    expect(spec.paths['/gui']).toBeDefined();
    expect(spec.paths['/gui/relays/{channel}/toggle']).toBeDefined();
    expect(spec.paths['/gui/schedules']).toBeDefined();
    expect(spec.paths['/openapi.json']).toBeDefined();

    const queueResponseSchema = spec.paths['/api/queue/next'].get.responses['200'].content['application/json'].schema;
    expect(queueResponseSchema.oneOf).toBeDefined();
    expect(queueResponseSchema.oneOf).toHaveLength(2);

    const queueExamples = spec.paths['/api/queue/next'].get.responses['200'].content['application/json'].examples;
    expect(queueExamples.relayCommand.value.command.action).toBeDefined();
    expect(queueExamples.scheduleUpdate.value.command.type).toBe('schedule_update');
  });
});
