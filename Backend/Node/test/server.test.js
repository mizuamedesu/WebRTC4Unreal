'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { once } = require('node:events');
const { createCredentialBroker, loadConfig, validateConfig } = require('../server');

function testConfig(overrides = {}) {
  return {
    developerCredential: 'server-only-test-value',
    hostPrincipalId: '00000000-0000-0000-0000-000000000001',
    bindHost: '127.0.0.1',
    port: 0,
    bootstrapKey: 'test-bootstrap-key',
    flowApiBaseUrl: 'https://flow.example.test',
    credentialEndpoint: 'https://management.example.test/access-credentials',
    tokenTtlSeconds: 60,
    rateLimitPerMinute: 20,
    ...overrides,
  };
}

test('configuration refuses an externally bound server without a bootstrap key', () => {
  assert.throws(
    () => validateConfig(testConfig({ bindHost: '0.0.0.0', bootstrapKey: '' })),
    /P2P_BOOTSTRAP_KEY/,
  );
});

test('loadConfig clamps token TTL and does not invent a developer credential', () => {
  const config = loadConfig({ FLOW_TOKEN_TTL_SECONDS: '99999' });
  assert.equal(config.tokenTtlSeconds, 900);
  assert.equal(config.developerCredential, '');
});

test('issues a least-privilege join credential and revokes its opaque lease', async (t) => {
  const upstreamCalls = [];
  const fakeFetch = async (url, options) => {
    upstreamCalls.push({ url, method: options.method, body: options.body });
    if (options.method === 'POST') {
      return new Response(JSON.stringify({
        context_id: 'context-not-returned-to-client',
        headers: {
          'x-flow-principal': 'short-principal',
          'x-flow-timestamp': 'short-timestamp',
          'x-flow-signature': 'short-signature',
        },
      }), { status: 200, headers: { 'Content-Type': 'application/json' } });
    }
    return new Response(null, { status: 204 });
  };

  const broker = createCredentialBroker(testConfig(), fakeFetch);
  broker.server.listen(0, '127.0.0.1');
  await once(broker.server, 'listening');
  t.after(async () => {
    await broker.revokeAll();
    broker.server.close();
  });

  const address = broker.server.address();
  const baseUrl = `http://127.0.0.1:${address.port}`;
  const unauthorized = await fetch(`${baseUrl}/api/p2p/credentials/join`);
  assert.equal(unauthorized.status, 401);

  const issued = await fetch(`${baseUrl}/api/p2p/credentials/join`, {
    headers: { 'X-P2P-Bootstrap-Key': 'test-bootstrap-key' },
  });
  assert.equal(issued.status, 200);
  assert.match(issued.headers.get('cache-control'), /no-store/);
  const payload = await issued.json();
  assert.equal(payload.role, 'join');
  assert.match(payload.principal_id, /^[0-9a-f-]{36}$/i);
  assert.equal(payload.signature, 'short-signature');
  assert.ok(payload.lease_id);
  assert.equal(payload.context_id, undefined);

  const createBody = JSON.parse(upstreamCalls[0].body);
  assert.deepEqual(createBody.permissions.sort(), [
    'flow.room.join',
    'flow.signal.connect',
    'flow.turn.issue',
  ].sort());

  const revoked = await fetch(`${baseUrl}/api/p2p/leases/${payload.lease_id}`, {
    method: 'DELETE',
    headers: { 'X-P2P-Bootstrap-Key': 'test-bootstrap-key' },
  });
  assert.equal(revoked.status, 200);
  assert.equal(upstreamCalls.at(-1).method, 'DELETE');
});

test('creates official Flow WSS direct-first sessions with TURN fallback and keeps signed contexts alive', async (t) => {
  const roomId = '11111111-2222-4333-8444-555555555555';
  let credentialSequence = 0;
  const upstreamCalls = [];
  const fakeFetch = async (url, options) => {
    upstreamCalls.push({ url, method: options.method, body: options.body });
    if (url.startsWith('https://management.example.test/access-credentials')) {
      if (options.method === 'POST') {
        credentialSequence += 1;
        return new Response(JSON.stringify({
          context_id: `context-${credentialSequence}`,
          headers: {
            'x-flow-principal': `principal-${credentialSequence}`,
            'x-flow-timestamp': `timestamp-${credentialSequence}`,
            'x-flow-signature': `signature-${credentialSequence}`,
          },
        }), { status: 200, headers: { 'Content-Type': 'application/json' } });
      }
      return new Response(null, { status: 204 });
    }
    if (url === 'https://flow.example.test/v1/rooms' && options.method === 'POST') {
      return new Response(JSON.stringify({ id: roomId, name: 'Test Room', max_participants: 4 }), {
        status: 201,
        headers: { 'Content-Type': 'application/json' },
      });
    }
    if (url === `https://flow.example.test/v1/rooms/${roomId}/join` && options.method === 'POST') {
      return new Response(JSON.stringify({
        room_id: roomId,
        mode: 'p2p',
        connection: {
          type: 'p2p',
          protocol: 'flow-signaling.v1',
          urls: [`wss://flow.example.test/v1/signal/${roomId}`],
          asyncapi_urls: ['https://flow.example.test/asyncapi.json'],
          ice: {
            expires_at: '2099-01-01T00:00:00Z',
            ice_servers: [
              {
                urls: ['stun:flow.example.test:3478'],
              },
              {
                urls: [
                  'turn:flow.example.test:3478?transport=udp',
                  'turn:flow.example.test:3478?transport=tcp',
                ],
                username: `turn-user-${credentialSequence}`,
                credential: `turn-password-${credentialSequence}`,
              },
            ],
          },
        },
      }), { status: 200, headers: { 'Content-Type': 'application/json' } });
    }
    return new Response(JSON.stringify({ message: `Unexpected fake URL: ${url}` }), { status: 500 });
  };

  const broker = createCredentialBroker(testConfig(), fakeFetch);
  broker.server.listen(0, '127.0.0.1');
  await once(broker.server, 'listening');
  t.after(async () => {
    await broker.revokeAll();
    broker.server.close();
  });

  const address = broker.server.address();
  const baseUrl = `http://127.0.0.1:${address.port}`;
  const headers = { 'X-P2P-Bootstrap-Key': 'test-bootstrap-key' };
  const hostResponse = await fetch(`${baseUrl}/api/p2p/sessions/host?room_name=Test%20Room&max_participants=4`, { headers });
  assert.equal(hostResponse.status, 200);
  const hostSession = await hostResponse.json();
  assert.equal(hostSession.room_id, roomId);
  assert.equal(hostSession.role, 'host');
  assert.equal(hostSession.local_principal_id, '00000000-0000-0000-0000-000000000001');
  assert.equal(hostSession.host_principal_id, '00000000-0000-0000-0000-000000000001');
  assert.equal(hostSession.max_participants, 4);
  assert.equal(hostSession.relay_only, false);
  assert.equal(hostSession.ice_policy, 'all');
  assert.equal(hostSession.turn_fallback, true);
  assert.deepEqual(hostSession.stun_urls, ['stun:flow.example.test:3478']);
  assert.equal(hostSession.ice_expires_at, '2099-01-01T00:00:00Z');
  assert.deepEqual(hostSession.ice_servers, [
    { urls: ['stun:flow.example.test:3478'], username: null, credential: null },
    {
      urls: [
        'turn:flow.example.test:3478?transport=udp',
        'turn:flow.example.test:3478?transport=tcp',
      ],
      username: 'turn-user-1',
      credential: 'turn-password-1',
    },
  ]);
  assert.equal(hostSession.transport, 'webrtc-datachannel');
  assert.equal(hostSession.signal_url, `wss://flow.example.test/v1/signal/${roomId}`);
  assert.deepEqual(hostSession.signaling_urls, [hostSession.signal_url]);
  assert.equal(hostSession.protocol, 'flow-signaling.v1');
  assert.deepEqual(hostSession.asyncapi_urls, ['https://flow.example.test/asyncapi.json']);
  assert.equal(hostSession.principal, undefined);
  assert.equal(hostSession.signature, undefined);
  assert.deepEqual(hostSession.signaling_auth, {
    type: 'signed_context',
    principal_context: 'principal-1',
    timestamp: 'timestamp-1',
    signature: 'signature-1',
    expires_in_seconds: 60,
    lease_id: hostSession.signaling_auth.lease_id,
  });
  assert.ok(hostSession.signaling_auth.lease_id);
  assert.equal(broker.leases.size, 1, 'host signed context remains valid for WSS authentication');

  const joinResponse = await fetch(`${baseUrl}/api/p2p/sessions/join?room_id=${roomId}`, { headers });
  assert.equal(joinResponse.status, 200);
  const joinSession = await joinResponse.json();
  assert.equal(joinSession.room_id, roomId);
  assert.equal(joinSession.role, 'join');
  assert.match(joinSession.local_principal_id, /^[0-9a-f-]{36}$/i);
  assert.notEqual(joinSession.local_principal_id, joinSession.host_principal_id);
  assert.equal(joinSession.host_principal_id, '00000000-0000-0000-0000-000000000001');
  assert.equal(joinSession.max_participants, 4);
  assert.equal(joinSession.relay_only, false);
  assert.equal(joinSession.ice_policy, 'all');
  assert.equal(joinSession.turn_fallback, true);
  assert.deepEqual(joinSession.stun_urls, ['stun:flow.example.test:3478']);
  assert.equal(joinSession.principal, undefined);
  assert.equal(joinSession.turn.password, 'turn-password-2');
  assert.equal(joinSession.signaling_auth.principal_context, 'principal-2');
  assert.equal(joinSession.signaling_auth.signature, 'signature-2');
  assert.equal(broker.leases.size, 2, 'both signed contexts remain valid until authentication or expiry');

  assert.equal(upstreamCalls.filter((call) => call.method === 'POST'
    && call.url.startsWith('https://management.example.test')).length, 2);
  assert.equal(upstreamCalls.filter((call) => call.method === 'DELETE').length, 0);

  const createRoomCall = upstreamCalls.find((call) => call.url === 'https://flow.example.test/v1/rooms');
  const createRoomBody = JSON.parse(createRoomCall.body);
  assert.equal(createRoomBody.max_participants, 4);
  assert.deepEqual(createRoomBody.metadata.unreal, {
    transport: 'webrtc-datachannel',
    host_principal_id: '00000000-0000-0000-0000-000000000001',
    ice_policy: 'all',
    relay_only: false,
    turn_fallback: true,
    schema_version: 5,
  });

  assert.equal(upstreamCalls.some((call) => call.url === 'https://flow.example.test/v1/service-overview'), false);
  assert.equal(upstreamCalls.some((call) => call.method === 'GET'
    && call.url === `https://flow.example.test/v1/rooms/${roomId}`), false);

  const revokeResponse = await fetch(`${baseUrl}/api/p2p/revoke-all`, { method: 'POST', headers });
  assert.equal(revokeResponse.status, 200);
  assert.deepEqual(await revokeResponse.json(), { requested: 2, revoked: 2, failed: 0 });
  assert.equal(broker.leases.size, 0);
  assert.equal(upstreamCalls.filter((call) => call.method === 'DELETE').length, 2);
});
