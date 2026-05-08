const request = require('supertest');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const { createApp } = require('../src/app');

describe('garden controller api', () => {
  const token = 'test-token';
  const auth = `Basic ${Buffer.from('admin:password').toString('base64')}`;

  function build() {
    return createApp({ apiToken: token, guiUsername: 'admin', guiPassword: 'password' }).app;
  }

  test('health check', async () => {
    const res = await request(build()).get('/health');
    expect(res.status).toBe(200);
    expect(res.body.ok).toBe(true);
  });

  test('rejects unauthorized api calls', async () => {
    const res = await request(build()).get('/api/relays');
    expect(res.status).toBe(401);
  });

  test('publishes full controller state', async () => {
    const app = createApp({
      apiToken: token,
      guiUsername: 'admin',
      guiPassword: 'password',
      state: {
        relayState: [{ channel: 1, state: 'on' }, { channel: 2, state: 'off' }],
        queue: [{ id: '1', channel: 1, action: 'toggle', requestedBy: 'gui', createdAt: '2026-01-01T00:00:00.000Z' }],
        news: [],
        schedules: ['06:00Z -> relay 1 on', '06:15Z -> relay 1 off']
      }
    }).app;

    const res = await request(app).get('/api/state').set('x-api-token', token);
    expect(res.status).toBe(200);
    expect(res.body.relays).toEqual([{ channel: 1, state: 'on' }, { channel: 2, state: 'off' }]);
    expect(res.body.queueDepth).toBe(1);
    expect(res.body.schedules).toEqual(['06:00Z -> relay 1 on', '06:15Z -> relay 1 off']);
    expect(new Date(res.body.serverTime).toString()).not.toBe('Invalid Date');
  });

  test('queues command and microcontroller pulls it', async () => {
    const app = build();
    const queueRes = await request(app)
      .post('/api/commands')
      .set('x-api-token', token)
      .send({ channel: 1, action: 'on' });
    expect(queueRes.status).toBe(201);

    const nextRes = await request(app)
      .get('/api/queue/next')
      .set('x-api-token', token);
    expect(nextRes.status).toBe(200);
    expect(nextRes.body.command.channel).toBe(1);
    expect(nextRes.body.relay.state).toBe('on');

    const empty = await request(app)
      .get('/api/queue/next')
      .set('x-api-token', token);
    expect(empty.status).toBe(204);
  });

  test('validates command payload', async () => {
    const res = await request(build())
      .post('/api/commands')
      .set('x-api-token', token)
      .send({ channel: 9, action: 'on' });
    expect(res.status).toBe(400);
  });

  test('news feed create and read', async () => {
    const app = build();
    const createRes = await request(app)
      .post('/api/news')
      .set('x-api-token', token)
      .send({ title: 'Test', body: 'Body' });
    expect(createRes.status).toBe(201);

    const list = await request(app)
      .get('/api/news')
      .set('x-api-token', token);
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
  });

  test('gui toggle control queues a command', async () => {
    const app = createApp({ apiToken: token, guiUsername: 'admin', guiPassword: 'password' });

    const res = await request(app.app)
      .post('/gui/relays/1/toggle')
      .set('authorization', auth)
      .redirects(0);

    expect(res.status).toBe(303);
    expect(app.state.queue).toHaveLength(1);
    expect(app.state.queue[0]).toMatchObject({ channel: 1, action: 'toggle', requestedBy: 'gui-web' });
  });

  test('gui auth supports colons in password from env-style credentials', async () => {
    const app = createApp({ apiToken: token, guiUsername: 'admin', guiPassword: 'pa:ss:word' }).app;
    const colonPasswordAuth = `Basic ${Buffer.from('admin:pa:ss:word').toString('base64')}`;

    const authorized = await request(app).get('/gui').set('authorization', colonPasswordAuth);
    expect(authorized.status).toBe(200);
  });

  test('reads GUI credentials from process.env when config is not provided', async () => {
    process.env.GUI_USERNAME = 'test';
    process.env.GUI_PASSWORD = 'test';

    const app = createApp({ apiToken: token }).app;
    const envAuth = `Basic ${Buffer.from('test:test').toString('base64')}`;
    const authorized = await request(app).get('/gui').set('authorization', envAuth);

    expect(authorized.status).toBe(200);

    delete process.env.GUI_USERNAME;
    delete process.env.GUI_PASSWORD;
  });


  test('reads quoted GUI credentials from process.env', async () => {
    process.env.GUI_USERNAME = '"quoted-user"';
    process.env.GUI_PASSWORD = "'quoted-pass'";

    const app = createApp({ apiToken: token }).app;
    const envAuth = `Basic ${Buffer.from('quoted-user:quoted-pass').toString('base64')}`;
    const authorized = await request(app).get('/gui').set('authorization', envAuth);

    expect(authorized.status).toBe(200);

    delete process.env.GUI_USERNAME;
    delete process.env.GUI_PASSWORD;
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
    expect(spec.paths['/api/queue/next']).toBeDefined();
    expect(spec.paths['/api/news']).toBeDefined();
    expect(spec.paths['/gui']).toBeDefined();
    expect(spec.paths['/gui/relays/{channel}/toggle']).toBeDefined();
    expect(spec.paths['/openapi.json']).toBeDefined();
  });
});
