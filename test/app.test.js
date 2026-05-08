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
  });

  test('openapi spec includes implemented endpoints', () => {
    const specPath = path.join(__dirname, '..', 'openapi.yaml');
    const spec = yaml.load(fs.readFileSync(specPath, 'utf8'));
    expect(spec.paths['/api/relays']).toBeDefined();
    expect(spec.paths['/api/commands']).toBeDefined();
    expect(spec.paths['/api/queue/next']).toBeDefined();
    expect(spec.paths['/api/news']).toBeDefined();
    expect(spec.paths['/gui']).toBeDefined();
    expect(spec.paths['/openapi.json']).toBeDefined();
  });
});
