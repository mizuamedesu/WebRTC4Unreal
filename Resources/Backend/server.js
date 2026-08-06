'use strict';

const http = require('node:http');
const { randomUUID, timingSafeEqual } = require('node:crypto');
const { URL } = require('node:url');
const fs = require('node:fs');
const path = require('node:path');

function enableFileLogging(env = process.env) {
  const logFile = (env.P2P_BACKEND_LOG_FILE ?? '').trim();
  if (!logFile) return;

  fs.mkdirSync(path.dirname(logFile), { recursive: true });
  for (const level of ['log', 'error', 'warn']) {
    const original = console[level].bind(console);
    console[level] = (...parts) => {
      original(...parts);
      const message = parts.map((part) => {
        if (part instanceof Error) return part.stack ?? part.message;
        return typeof part === 'string' ? part : JSON.stringify(part);
      }).join(' ');
      fs.appendFileSync(logFile, `[${new Date().toISOString()}] ${level.toUpperCase()} ${message}\n`, 'utf8');
    };
  }
}

enableFileLogging();

const HOST_PERMISSIONS = [
  'flow.room.create',
  'flow.room.join',
  'flow.signal.connect',
  'flow.turn.issue',
];

const JOIN_PERMISSIONS = [
  'flow.room.join',
  'flow.signal.connect',
  'flow.turn.issue',
];

function integerSetting(value, fallback, min, max) {
  const parsed = Number.parseInt(value ?? '', 10);
  if (!Number.isFinite(parsed)) return fallback;
  return Math.min(max, Math.max(min, parsed));
}

function loadConfig(env = process.env) {
  return {
    developerCredential: env.HCF_DEVELOPER_CREDENTIAL ?? '',
    hostPrincipalId: env.HCF_PRINCIPAL_ID ?? '',
    bindHost: env.P2P_BIND_HOST ?? '127.0.0.1',
    port: integerSetting(env.P2P_BACKEND_PORT, 8787, 1, 65535),
    bootstrapKey: env.P2P_BOOTSTRAP_KEY ?? '',
    flowApiBaseUrl: (env.FLOW_API_BASE_URL ?? 'https://flow.heterocloud.mizuame.app').replace(/\/+$/, ''),
    credentialEndpoint: (env.HCF_CREDENTIAL_ENDPOINT ?? 'https://heterocloud.mizuame.app/api/v1/flow/v1/access-credentials').replace(/\/+$/, ''),
    tokenTtlSeconds: integerSetting(env.FLOW_TOKEN_TTL_SECONDS, 300, 60, 900),
    rateLimitPerMinute: integerSetting(env.P2P_RATE_LIMIT_PER_MINUTE, 20, 2, 300),
  };
}

function validateConfig(config) {
  if (!config.developerCredential) {
    throw new Error('HCF_DEVELOPER_CREDENTIAL is required and must only be set on the backend.');
  }
  if (!config.hostPrincipalId) {
    throw new Error('HCF_PRINCIPAL_ID is required.');
  }
  const loopbackHosts = new Set(['127.0.0.1', '::1', 'localhost']);
  if (!loopbackHosts.has(config.bindHost) && !config.bootstrapKey) {
    throw new Error('P2P_BOOTSTRAP_KEY is required when P2P_BIND_HOST is not loopback.');
  }
}

function jsonResponse(response, statusCode, payload, extraHeaders = {}) {
  const body = JSON.stringify(payload);
  response.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store, private, max-age=0',
    Pragma: 'no-cache',
    ...extraHeaders,
  });
  response.end(body);
}

async function readJsonSafe(response) {
  const text = await response.text();
  if (!text) return {};
  try {
    return JSON.parse(text);
  } catch {
    return { message: text.slice(0, 256) };
  }
}

function getObjectHeader(headers, targetName) {
  if (!headers || typeof headers !== 'object') return '';
  const target = targetName.toLowerCase();
  const entry = Object.entries(headers).find(([name]) => name.toLowerCase() === target);
  return entry && typeof entry[1] === 'string' ? entry[1] : '';
}

function secretsMatch(expected, actual) {
  const expectedBuffer = Buffer.from(expected);
  const actualBuffer = Buffer.from(actual ?? '');
  return expectedBuffer.length === actualBuffer.length && timingSafeEqual(expectedBuffer, actualBuffer);
}

function createCredentialBroker(config, fetchImpl = globalThis.fetch) {
  validateConfig(config);
  if (typeof fetchImpl !== 'function') {
    throw new Error('A Fetch API implementation is required. Use Node.js 20 or newer.');
  }

  const leases = new Map();
  const rateBuckets = new Map();
  const createdRooms = new Map();

  function isAuthorized(request) {
    return !config.bootstrapKey || secretsMatch(config.bootstrapKey, request.headers['x-p2p-bootstrap-key']);
  }

  function consumeRateLimit(request) {
    const address = request.socket.remoteAddress ?? 'unknown';
    const now = Date.now();
    const existing = rateBuckets.get(address);
    if (!existing || now - existing.startedAt >= 60_000) {
      rateBuckets.set(address, { startedAt: now, count: 1 });
      return true;
    }
    existing.count += 1;
    return existing.count <= config.rateLimitPerMinute;
  }

  async function deleteContext(contextId) {
    const response = await fetchImpl(`${config.credentialEndpoint}/${encodeURIComponent(contextId)}`, {
      method: 'DELETE',
      headers: {
        Authorization: `Bearer ${config.developerCredential}`,
        Accept: 'application/json',
      },
      signal: AbortSignal.timeout(10_000),
    });
    if (!response.ok && response.status !== 404) {
      throw new Error(`Credential revocation failed with HTTP ${response.status}`);
    }
  }

  async function revokeLease(leaseId) {
    const lease = leases.get(leaseId);
    if (!lease) return false;
    leases.delete(leaseId);
    clearTimeout(lease.timer);
    await deleteContext(lease.contextId);
    return true;
  }

  async function revokeAll() {
    const ids = [...leases.keys()];
    const results = await Promise.allSettled(ids.map(revokeLease));
    return {
      requested: ids.length,
      revoked: results.filter((result) => result.status === 'fulfilled' && result.value).length,
      failed: results.filter((result) => result.status === 'rejected').length,
    };
  }

  async function issueCredential(role) {
    const isHost = role === 'host';
    const principalId = isHost ? config.hostPrincipalId : randomUUID();
    const permissions = isHost ? HOST_PERMISSIONS : JOIN_PERMISSIONS;
    const response = await fetchImpl(config.credentialEndpoint, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${config.developerCredential}`,
        'Content-Type': 'application/json',
        Accept: 'application/json',
      },
      body: JSON.stringify({
        principal_id: principalId,
        permissions,
        expires_in_seconds: config.tokenTtlSeconds,
      }),
      signal: AbortSignal.timeout(10_000),
    });
    const context = await readJsonSafe(response);
    if (!response.ok) {
      const upstreamMessage = context?.error?.message ?? context?.message ?? 'Unknown upstream error';
      throw new Error(`Credential issuance failed with HTTP ${response.status}: ${upstreamMessage}`);
    }

    const principal = getObjectHeader(context.headers, 'x-flow-principal');
    const timestamp = getObjectHeader(context.headers, 'x-flow-timestamp');
    const signature = getObjectHeader(context.headers, 'x-flow-signature');
    if (!context.context_id || !principal || !timestamp || !signature) {
      throw new Error('Credential service response is missing context_id or X-Flow headers.');
    }

    const leaseId = randomUUID();
    const timer = setTimeout(() => {
      revokeLease(leaseId).catch((error) => {
        console.error(`[credential-broker] automatic revocation failed: ${error.message}`);
      });
    }, config.tokenTtlSeconds * 1000);
    timer.unref();
    leases.set(leaseId, { contextId: context.context_id, timer });

    return {
      role,
      principal_id: principalId,
      api_base_url: config.flowApiBaseUrl,
      principal,
      timestamp,
      signature,
      expires_in_seconds: config.tokenTtlSeconds,
      lease_id: leaseId,
    };
  }

  function flowHeaders(credential) {
    return {
      Accept: 'application/json',
      'Content-Type': 'application/json',
      'X-Flow-Principal': credential.principal,
      'X-Flow-Timestamp': credential.timestamp,
      'X-Flow-Signature': credential.signature,
    };
  }

  async function flowRequest(path, credential, { method = 'GET', body } = {}) {
    const response = await fetchImpl(`${config.flowApiBaseUrl}${path}`, {
      method,
      headers: flowHeaders(credential),
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: AbortSignal.timeout(10_000),
    });
    const payload = await readJsonSafe(response);
    if (!response.ok) {
      const upstreamMessage = payload?.error?.message ?? payload?.message ?? 'Unknown Flow error';
      throw new Error(`Flow request ${method} ${path} failed with HTTP ${response.status}: ${upstreamMessage}`);
    }
    return payload;
  }

  async function withSessionCredential(role, operation) {
    const credential = await issueCredential(role);
    try {
      return await operation(credential);
    } catch (error) {
      // A signalling signed_context must remain valid until the Unreal client has
      // authenticated its official Flow WebSocket. Revoke immediately only when
      // REST session preparation failed; successful leases expire automatically.
      try {
        await revokeLease(credential.lease_id);
      } catch (revokeError) {
        console.error(`[credential-broker] failed to revoke rejected session lease: ${revokeError.message}`);
      }
      throw error;
    }
  }

  function requireP2PConnection(joinResponse) {
    const connection = joinResponse?.connection;
    if (!connection || connection.type !== 'p2p'
      || connection.protocol !== 'flow-signaling.v1'
      || !Array.isArray(connection.urls) || connection.urls.length === 0
      || !connection.urls.every((url) => typeof url === 'string' && /^wss:\/\//i.test(url))
      || !Array.isArray(connection.asyncapi_urls) || connection.asyncapi_urls.length === 0
      || !connection.asyncapi_urls.every((url) => typeof url === 'string' && /^https:\/\//i.test(url))) {
      throw new Error('Flow join response did not contain a flow-signaling.v1 P2P WebSocket connection.');
    }

    const ice = connection.ice;
    if (!ice || !Array.isArray(ice.ice_servers) || ice.ice_servers.length === 0
      || typeof ice.expires_at !== 'string' || ice.expires_at.length === 0) {
      throw new Error('Flow join response did not contain the required ICE configuration.');
    }

    const iceServers = [];
    const stunUrls = [];
    const turnUrls = [];
    let turnUsername = '';
    let turnCredential = '';
    for (const [index, server] of ice.ice_servers.entries()) {
      if (!server || !Array.isArray(server.urls) || server.urls.length === 0
        || !server.urls.every((url) => typeof url === 'string' && url.length > 0)) {
        throw new Error(`Flow ICE server ${index} did not contain usable URLs.`);
      }

      const username = typeof server.username === 'string' ? server.username : '';
      const credential = typeof server.credential === 'string' ? server.credential : '';
      const serverStunUrls = server.urls.filter((url) => /^stuns?:/i.test(url));
      const serverTurnUrls = server.urls.filter((url) => /^turns?:/i.test(url));
      stunUrls.push(...serverStunUrls);
      if (serverTurnUrls.length > 0) {
        if (!username || !credential) {
          throw new Error(`Flow TURN server ${index} did not contain authenticated credentials.`);
        }
        if (turnUsername && (turnUsername !== username || turnCredential !== credential)) {
          throw new Error('Flow returned multiple TURN credential groups unsupported by this Unreal adapter.');
        }
        turnUsername = username;
        turnCredential = credential;
        turnUrls.push(...serverTurnUrls);
      }

      iceServers.push({
        urls: [...server.urls],
        username: username || null,
        credential: credential || null,
      });
    }
    if (stunUrls.length === 0 || turnUrls.length === 0 || !turnUsername || !turnCredential) {
      throw new Error('Flow join response did not contain direct-first STUN and authenticated TURN fallback data.');
    }

    return {
      signalingUrls: connection.urls,
      protocol: connection.protocol,
      asyncApiUrls: connection.asyncapi_urls,
      iceServers,
      stunUrls: [...new Set(stunUrls)],
      turn: {
        urls: [...new Set(turnUrls)],
        username: turnUsername,
        password: turnCredential,
        expires_at: ice.expires_at,
      },
    };
  }

  function sessionPayload(room, credential, connection) {
    return {
      room_id: room.id,
      room_name: room.name ?? room.id,
      role: credential.role,
      local_principal_id: credential.principal_id,
      host_principal_id: config.hostPrincipalId,
      max_participants: Number.isInteger(room.max_participants) ? room.max_participants : 0,
      signal_url: connection.signalingUrls[0],
      signaling_urls: connection.signalingUrls,
      protocol: connection.protocol,
      asyncapi_urls: connection.asyncApiUrls,
      signaling_auth: {
        type: 'signed_context',
        principal_context: credential.principal,
        timestamp: credential.timestamp,
        signature: credential.signature,
        expires_in_seconds: credential.expires_in_seconds,
        lease_id: credential.lease_id,
      },
      ice_servers: connection.iceServers,
      ice_expires_at: connection.turn.expires_at,
      // Legacy fields keep already-built Unreal clients compatible while the
      // broker consumes Flow API 0.1.13's browser-native ICE server array.
      stun_urls: connection.stunUrls,
      turn: connection.turn,
      ice_policy: 'all',
      relay_only: false,
      turn_fallback: true,
      transport: 'webrtc-datachannel',
    };
  }

  async function createHostSession(roomName, requestedMaxParticipants) {
    const maxParticipants = integerSetting(requestedMaxParticipants, 4, 2, 1000);
    return withSessionCredential('host', async (credential) => {
      const room = await flowRequest('/v1/rooms', credential, {
        method: 'POST',
        body: {
          mode: 'p2p',
          name: roomName || 'Unreal P2P Room',
          max_participants: maxParticipants,
          metadata: {
            unreal: {
              transport: 'webrtc-datachannel',
              host_principal_id: config.hostPrincipalId,
              ice_policy: 'all',
              relay_only: false,
              turn_fallback: true,
              schema_version: 5,
            },
          },
        },
      });
      createdRooms.set(room.id, {
        id: room.id,
        name: room.name ?? room.id,
        max_participants: room.max_participants ?? maxParticipants,
      });
      const joined = await flowRequest(`/v1/rooms/${encodeURIComponent(room.id)}/join`, credential, {
        method: 'POST',
        body: { can_publish: true, can_subscribe: true, display_name: 'Unreal listen host' },
      });
      return sessionPayload(room, credential, requireP2PConnection(joined));
    });
  }

  async function createJoinSession(roomId) {
    return withSessionCredential('join', async (credential) => {
      const joined = await flowRequest(`/v1/rooms/${encodeURIComponent(roomId)}/join`, credential, {
        method: 'POST',
        body: { can_publish: true, can_subscribe: true, display_name: 'Unreal joining client' },
      });
      if (typeof joined?.room_id !== 'string' || joined.room_id !== roomId || joined.mode !== 'p2p') {
        throw new Error('Flow join response did not match the requested P2P room.');
      }
      const knownRoom = createdRooms.get(joined.room_id)
        ?? { id: joined.room_id, name: joined.room_id, max_participants: 0 };
      return sessionPayload(knownRoom, credential,
        requireP2PConnection(joined));
    });
  }

  const server = http.createServer(async (request, response) => {
    try {
      const url = new URL(request.url ?? '/', `http://${request.headers.host ?? 'localhost'}`);

      if (request.method === 'GET' && url.pathname === '/health') {
        jsonResponse(response, 200, { ok: true, active_leases: leases.size });
        return;
      }

      if (!isAuthorized(request)) {
        jsonResponse(response, 401, { error: 'unauthorized', message: 'Missing or invalid bootstrap key.' });
        return;
      }

      const credentialMatch = url.pathname.match(/^\/api\/p2p\/credentials\/(host|join)$/);
      if (request.method === 'GET' && credentialMatch) {
        if (!consumeRateLimit(request)) {
          jsonResponse(response, 429, { error: 'rate_limited', message: 'Too many credential requests.' }, { 'Retry-After': '60' });
          return;
        }
        jsonResponse(response, 200, await issueCredential(credentialMatch[1]));
        return;
      }

      if (request.method === 'GET' && url.pathname === '/api/p2p/sessions/host') {
        if (!consumeRateLimit(request)) {
          jsonResponse(response, 429, { error: 'rate_limited', message: 'Too many session requests.' }, { 'Retry-After': '60' });
          return;
        }
        jsonResponse(response, 200, await createHostSession(
          url.searchParams.get('room_name') ?? '',
          url.searchParams.get('max_participants') ?? '4'));
        return;
      }

      if (request.method === 'GET' && url.pathname === '/api/p2p/sessions/join') {
        if (!consumeRateLimit(request)) {
          jsonResponse(response, 429, { error: 'rate_limited', message: 'Too many session requests.' }, { 'Retry-After': '60' });
          return;
        }
        const roomId = url.searchParams.get('room_id') ?? '';
        if (!/^[0-9a-f-]{20,64}$/i.test(roomId)) {
          jsonResponse(response, 400, { error: 'invalid_room_id', message: 'A valid room ID is required.' });
          return;
        }
        jsonResponse(response, 200, await createJoinSession(roomId));
        return;
      }

      const leaseMatch = url.pathname.match(/^\/api\/p2p\/leases\/([0-9a-f-]+)$/i);
      if (request.method === 'DELETE' && leaseMatch) {
        const revoked = await revokeLease(leaseMatch[1]);
        jsonResponse(response, revoked ? 200 : 404, { revoked });
        return;
      }

      if (request.method === 'POST' && url.pathname === '/api/p2p/revoke-all') {
        jsonResponse(response, 200, await revokeAll());
        return;
      }

      jsonResponse(response, 404, {
        error: 'not_found',
        endpoints: [
          'GET /health',
          'GET /api/p2p/sessions/host',
          'GET /api/p2p/sessions/join?room_id={room_id}',
          'GET /api/p2p/credentials/host',
          'GET /api/p2p/credentials/join',
          'DELETE /api/p2p/leases/{lease_id}',
        ],
      });
    } catch (error) {
      console.error(`[credential-broker] request failed: ${error.message}`);
      jsonResponse(response, 502, { error: 'credential_broker_error', message: error.message });
    }
  });

  return { server, revokeAll, leases };
}

async function main() {
  const config = loadConfig();
  const broker = createCredentialBroker(config);
  broker.server.on('error', (error) => {
    console.error(`[credential-broker] server error: ${error.message}`);
    process.exit(2);
  });
  broker.server.listen(config.port, config.bindHost, () => {
    console.log(`[credential-broker] listening on http://${config.bindHost}:${config.port}`);
  });

  let shuttingDown = false;
  let parentWatch = null;
  const shutdown = async (signal) => {
    if (shuttingDown) return;
    shuttingDown = true;
    if (parentWatch) clearInterval(parentWatch);
    console.log(`[credential-broker] ${signal}; revoking short-lived credentials`);
    broker.server.close();
    await broker.revokeAll();
    process.exit(0);
  };
  process.on('SIGINT', () => void shutdown('SIGINT'));
  process.on('SIGTERM', () => void shutdown('SIGTERM'));

  const parentPid = Number.parseInt(process.env.P2P_BACKEND_PARENT_PID ?? '', 10);
  if (Number.isInteger(parentPid) && parentPid > 0) {
    parentWatch = setInterval(() => {
      try {
        process.kill(parentPid, 0);
      } catch {
        void shutdown('parent-exit');
      }
    }, 1000);
  }
}

if (require.main === module) {
  main().catch((error) => {
    console.error(`[credential-broker] startup failed: ${error.message}`);
    process.exit(1);
  });
}

module.exports = {
  HOST_PERMISSIONS,
  JOIN_PERMISSIONS,
  createCredentialBroker,
  loadConfig,
  validateConfig,
};
