import { DirectRoom } from "./direct-room";
import { createFlowHostSession, createFlowJoinSession, flowConfigured } from "./flow";
import { RealtimeRoom } from "./room";
import {
  apiError,
  bearerToken,
  getInteger,
  getString,
  isJsonObject,
  noStoreJson,
  randomToken,
  readJsonObject,
  secureEqual,
  sha256Hex,
  structuredLog
} from "./security";
import type {
  DirectParticipantView,
  JsonObject,
  ParticipantView,
  RealtimeResult,
  WorkerEnv
} from "./types";

export { DirectRoom, RealtimeRoom };

// Flow currently issues RFC 9562 UUIDv7 room identifiers while Web Crypto emits UUIDv4.
const ROOM_ID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
// FGuid::NewGuid() is a 128-bit GUID but does not promise RFC version/variant bits.
const PARTICIPANT_ID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

function sfuConfigured(env: WorkerEnv): boolean {
  return Boolean(env.CALLS_APP_ID && env.CALLS_APP_SECRET && env.CLIENT_ACCESS_KEY);
}

function turnConfigured(env: WorkerEnv): boolean {
  return Boolean(env.TURN_KEY_ID && env.TURN_KEY_API_TOKEN);
}

function directConfigured(env: WorkerEnv): boolean {
  return Boolean(env.CLIENT_ACCESS_KEY && turnConfigured(env));
}

function requestedDirectParticipantId(request: Request): string | Response {
  const requested = request.headers.get("X-WebRTC4Unreal-Participant-Id")?.trim() ?? "";
  if (!requested) return crypto.randomUUID();
  return PARTICIPANT_ID_PATTERN.test(requested)
    ? requested
    : apiError(400, "invalid_participant_id", "Requested participant ID must be a UUID");
}

function roomStub(env: WorkerEnv, roomId: string): DurableObjectStub<RealtimeRoom> {
  return env.ROOMS.getByName(roomId);
}

function directRoomStub(env: WorkerEnv, roomId: string): DurableObjectStub<DirectRoom> {
  return env.P2P_ROOMS.getByName(roomId);
}

async function bootstrapAuthorized(request: Request, env: WorkerEnv): Promise<boolean> {
  const configured = env.CLIENT_ACCESS_KEY ?? "";
  const supplied = request.headers.get("X-P2P-Bootstrap-Key")?.trim() ?? "";
  return Boolean(configured && supplied) && secureEqual(configured, supplied);
}

async function internalRequest(
  stub: DurableObjectStub<RealtimeRoom> | DurableObjectStub<DirectRoom>,
  pathname: string,
  method: string,
  body?: JsonObject,
  participantId = "",
  participantHash = "",
  upgrade = ""
): Promise<Response> {
  const headers = new Headers({ Accept: "application/json" });
  if (body) headers.set("Content-Type", "application/json");
  if (participantId) headers.set("X-Participant-ID", participantId);
  if (participantHash) headers.set("X-Participant-Hash", participantHash);
  if (upgrade) headers.set("Upgrade", upgrade);
  const init: RequestInit = { method, headers };
  if (body) init.body = JSON.stringify(body);
  return stub.fetch(new Request(`https://room.internal${pathname}`, init));
}

async function parseInternal<T>(response: Response): Promise<T> {
  const body: unknown = await response.json();
  if (!response.ok) {
    const message = isJsonObject(body) && isJsonObject(body.error)
      ? getString(body.error, "message")
      : "Room coordination failed";
    throw new Error(`room_http_${response.status}:${message}`);
  }
  return body as T;
}

async function participantView(
  stub: DurableObjectStub<RealtimeRoom>,
  participantId: string,
  participantToken: string
): Promise<{ view: ParticipantView; tokenHash: string } | Response> {
  if (!PARTICIPANT_ID_PATTERN.test(participantId) || participantToken.length < 32) {
    return apiError(401, "participant_unauthorized", "Participant token is invalid");
  }
  const tokenHash = await sha256Hex(participantToken);
  const response = await internalRequest(
    stub, "/internal/participant", "GET", undefined, participantId, tokenHash
  );
  if (!response.ok) {
    return apiError(401, "participant_unauthorized", "Participant token is invalid");
  }
  return { view: await response.json() as ParticipantView, tokenHash };
}

async function directParticipantView(
  stub: DurableObjectStub<DirectRoom>,
  participantId: string,
  participantToken: string
): Promise<{ view: DirectParticipantView; tokenHash: string } | Response> {
  if (!PARTICIPANT_ID_PATTERN.test(participantId) || participantToken.length < 32) {
    return apiError(401, "participant_unauthorized", "Participant token is invalid");
  }
  const tokenHash = await sha256Hex(participantToken);
  const response = await internalRequest(
    stub, "/internal/participant", "GET", undefined, participantId, tokenHash
  );
  if (!response.ok) {
    return apiError(401, "participant_unauthorized", "Participant token is invalid");
  }
  return { view: await response.json() as DirectParticipantView, tokenHash };
}

async function callRealtime(
  env: WorkerEnv,
  path: string,
  method: "POST" | "PUT",
  body?: JsonObject
): Promise<RealtimeResult> {
  if (!env.CALLS_APP_ID || !env.CALLS_APP_SECRET) {
    return {
      status: 503,
      body: { error: { code: "realtime_not_configured", message: "Realtime app credentials are not configured" } }
    };
  }
  const base = (env.CALLS_API_BASE || "https://rtc.live.cloudflare.com/v1").replace(/\/+$/, "");
  const headers = new Headers({
    Accept: "application/json",
    Authorization: `Bearer ${env.CALLS_APP_SECRET}`
  });
  const init: RequestInit = { method, headers };
  if (body) {
    headers.set("Content-Type", "application/json");
    init.body = JSON.stringify(body);
  }
  const response = await fetch(`${base}/apps/${encodeURIComponent(env.CALLS_APP_ID)}${path}`, init);
  const text = await response.text();
  let parsed: unknown = {};
  try {
    parsed = text ? JSON.parse(text) : {};
  } catch {
    parsed = { error: { code: "realtime_invalid_json", message: "Realtime API returned invalid JSON" } };
  }
  const resultBody = isJsonObject(parsed)
    ? parsed
    : { error: { code: "realtime_invalid_response", message: "Realtime API returned an invalid response" } };
  if (!response.ok) {
    const nestedError = isJsonObject(resultBody.error) ? resultBody.error : {};
    const upstreamCode = getString(resultBody, "errorCode")
      || getString(nestedError, "code")
      || `realtime_http_${response.status}`;
    const upstreamMessage = getString(resultBody, "errorDescription")
      || getString(nestedError, "message")
      || `Realtime API failed with HTTP ${response.status}`;
    structuredLog("realtime_api_failed", {
      path,
      status: response.status,
      code: upstreamCode.slice(0, 128),
      message: upstreamMessage.slice(0, 256)
    });
    return {
      status: response.status,
      body: {
        error: {
          code: upstreamCode.slice(0, 128),
          message: upstreamMessage.slice(0, 512)
        }
      }
    };
  }
  return { status: response.status, body: resultBody };
}

function realtimeResponse(result: RealtimeResult): Response {
  return noStoreJson(result.body, result.status);
}

function logDataChannelResult(
  direction: "publish_downlink" | "subscribe_downlink" | "publish_uplink" | "subscribe_uplink",
  result: RealtimeResult
): void {
  const channels = Array.isArray(result.body.dataChannels)
    ? result.body.dataChannels
    : Array.isArray(result.body.datachannels)
      ? result.body.datachannels
      : [];
  const firstChannel = channels.length > 0 && isJsonObject(channels[0]) ? channels[0] : {};
  const description = isJsonObject(result.body.sessionDescription)
    ? result.body.sessionDescription
    : {};
  structuredLog("realtime_datachannel_result", {
    direction,
    status: result.status,
    channel_id: typeof firstChannel.id === "number" ? firstChannel.id : -1,
    requires_renegotiation: result.body.requiresImmediateRenegotiation === true,
    offer_present: typeof description.sdp === "string" && description.sdp.length > 0
  });
}

async function createRoom(request: Request, env: WorkerEnv, origin: string): Promise<Response> {
  if (!sfuConfigured(env)) {
    return apiError(503, "worker_not_configured", "Realtime Worker secrets are not configured");
  }
  if (!await bootstrapAuthorized(request, env)) {
    return apiError(401, "bootstrap_unauthorized", "Client bootstrap key is invalid");
  }
  const body = await readJsonObject(request);
  const roomId = crypto.randomUUID();
  const participantId = crypto.randomUUID();
  const participantToken = randomToken();
  const tokenHash = await sha256Hex(participantToken);
  const roomName = getString(body, "room_name").slice(0, 128) || "Unreal Realtime Room";
  const maxParticipants = Math.min(100, Math.max(2, getInteger(body, "max_participants", 4)));
  const ttlSeconds = Math.min(86400, Math.max(300, Number(env.ROOM_TTL_SECONDS || "21600") || 21600));
  const stub = roomStub(env, roomId);
  const created = await internalRequest(stub, "/internal/create", "POST", {
    room_id: roomId,
    room_name: roomName,
    host_id: participantId,
    host_token_hash: tokenHash,
    max_participants: maxParticipants,
    ttl_seconds: ttlSeconds
  });
  if (!created.ok) return new Response(created.body, created);
  structuredLog("room_api_created", { room_id: roomId, max_participants: maxParticipants });
  return noStoreJson({
    protocol: "cloudflare-realtime.v1",
    room_id: roomId,
    room_name: roomName,
    participant_id: participantId,
    participant_token: participantToken,
    host_id: participantId,
    role: "host",
    worker_url: origin,
    signal_url: `${origin.replace(/^http/i, "ws")}/v1/rooms/${roomId}/connect`,
    max_participants: maxParticipants,
    relay_only: true
  }, 201);
}

async function joinRoom(request: Request, env: WorkerEnv, roomId: string, origin: string): Promise<Response> {
  if (!sfuConfigured(env)) {
    return apiError(503, "worker_not_configured", "Realtime Worker secrets are not configured");
  }
  if (!await bootstrapAuthorized(request, env)) {
    return apiError(401, "bootstrap_unauthorized", "Client bootstrap key is invalid");
  }
  if (!ROOM_ID_PATTERN.test(roomId)) {
    return apiError(400, "invalid_room_id", "Room ID is invalid");
  }
  const participantId = crypto.randomUUID();
  const participantToken = randomToken();
  const tokenHash = await sha256Hex(participantToken);
  const joined = await internalRequest(roomStub(env, roomId), "/internal/join", "POST", {
    participant_id: participantId,
    token_hash: tokenHash
  });
  if (!joined.ok) return new Response(joined.body, joined);
  const room = await joined.json() as JsonObject;
  structuredLog("room_api_joined", { room_id: roomId, participant_id: participantId });
  return noStoreJson({
    protocol: "cloudflare-realtime.v1",
    room_id: roomId,
    room_name: getString(room, "room_name"),
    participant_id: participantId,
    participant_token: participantToken,
    host_id: getString(room, "host_id"),
    role: "client",
    worker_url: origin,
    signal_url: `${origin.replace(/^http/i, "ws")}/v1/rooms/${roomId}/connect`,
    max_participants: getInteger(room, "max_participants", 4),
    relay_only: true
  }, 201);
}

async function connectRoom(request: Request, env: WorkerEnv, roomId: string): Promise<Response> {
  if (!ROOM_ID_PATTERN.test(roomId)) return apiError(400, "invalid_room_id", "Room ID is invalid");
  const participantId = request.headers.get("X-WebRTC4Unreal-Participant")?.trim() ?? "";
  const token = bearerToken(request);
  if (!PARTICIPANT_ID_PATTERN.test(participantId) || token.length < 32) {
    return apiError(401, "participant_unauthorized", "Participant token is invalid");
  }
  const tokenHash = await sha256Hex(token);
  return internalRequest(
    roomStub(env, roomId),
    "/internal/connect",
    "GET",
    undefined,
    participantId,
    tokenHash,
    request.headers.get("Upgrade") ?? ""
  );
}

async function createDirectRoom(request: Request, env: WorkerEnv, origin: string): Promise<Response> {
  if (!directConfigured(env)) {
    return apiError(503, "worker_not_configured", "Direct P2P Worker secrets are not configured");
  }
  if (!await bootstrapAuthorized(request, env)) {
    return apiError(401, "bootstrap_unauthorized", "Client bootstrap key is invalid");
  }
  const body = await readJsonObject(request);
  const requestedRoomId = getString(body, "room_id");
  if (requestedRoomId && !ROOM_ID_PATTERN.test(requestedRoomId)) {
    return apiError(400, "invalid_room_id", "Requested room ID must be a UUID");
  }
  const roomId = requestedRoomId || crypto.randomUUID();
  const participantId = requestedDirectParticipantId(request);
  if (participantId instanceof Response) return participantId;
  const participantToken = randomToken();
  const tokenHash = await sha256Hex(participantToken);
  const roomName = getString(body, "room_name").slice(0, 128) || "Unreal Direct P2P Room";
  const maxParticipants = Math.min(100, Math.max(2, getInteger(body, "max_participants", 4)));
  const ttlSeconds = Math.min(86400, Math.max(300, Number(env.ROOM_TTL_SECONDS || "21600") || 21600));
  const created = await internalRequest(directRoomStub(env, roomId), "/internal/create", "POST", {
    room_id: roomId,
    room_name: roomName,
    host_id: participantId,
    host_token_hash: tokenHash,
    max_participants: maxParticipants,
    ttl_seconds: ttlSeconds
  });
  if (!created.ok) return new Response(created.body, created);
  structuredLog("direct_room_api_created", { room_id: roomId, max_participants: maxParticipants });
  return noStoreJson({
    protocol: "cloudflare-direct.v1",
    room_id: roomId,
    room_name: roomName,
    participant_id: participantId,
    participant_token: participantToken,
    host_id: participantId,
    role: "host",
    worker_url: origin,
    signal_url: `${origin.replace(/^http/i, "ws")}/v1/p2p/rooms/${roomId}/connect`,
    max_participants: maxParticipants,
    relay_only: false,
    turn_fallback: true
  }, 201);
}

async function joinDirectRoom(
  request: Request,
  env: WorkerEnv,
  roomId: string,
  origin: string
): Promise<Response> {
  if (!directConfigured(env)) {
    return apiError(503, "worker_not_configured", "Direct P2P Worker secrets are not configured");
  }
  if (!await bootstrapAuthorized(request, env)) {
    return apiError(401, "bootstrap_unauthorized", "Client bootstrap key is invalid");
  }
  if (!ROOM_ID_PATTERN.test(roomId)) {
    return apiError(400, "invalid_room_id", "Room ID is invalid");
  }
  const participantId = requestedDirectParticipantId(request);
  if (participantId instanceof Response) return participantId;
  const participantToken = randomToken();
  const tokenHash = await sha256Hex(participantToken);
  const joined = await internalRequest(directRoomStub(env, roomId), "/internal/join", "POST", {
    participant_id: participantId,
    token_hash: tokenHash
  });
  if (!joined.ok) return new Response(joined.body, joined);
  const room = await joined.json() as JsonObject;
  structuredLog("direct_room_api_joined", { room_id: roomId, participant_id: participantId });
  return noStoreJson({
    protocol: "cloudflare-direct.v1",
    room_id: roomId,
    room_name: getString(room, "room_name"),
    participant_id: participantId,
    participant_token: participantToken,
    host_id: getString(room, "host_id"),
    role: "client",
    worker_url: origin,
    signal_url: `${origin.replace(/^http/i, "ws")}/v1/p2p/rooms/${roomId}/connect`,
    max_participants: getInteger(room, "max_participants", 4),
    relay_only: false,
    turn_fallback: true
  }, 201);
}

async function connectDirectRoom(request: Request, env: WorkerEnv, roomId: string): Promise<Response> {
  if (!ROOM_ID_PATTERN.test(roomId)) return apiError(400, "invalid_room_id", "Room ID is invalid");
  const participantId = request.headers.get("X-WebRTC4Unreal-Participant")?.trim() ?? "";
  const token = bearerToken(request);
  if (!PARTICIPANT_ID_PATTERN.test(participantId) || token.length < 32) {
    return apiError(401, "participant_unauthorized", "Participant token is invalid");
  }
  return internalRequest(
    directRoomStub(env, roomId),
    "/internal/connect",
    "GET",
    undefined,
    participantId,
    await sha256Hex(token),
    request.headers.get("Upgrade") ?? ""
  );
}

function sanitizeIceServer(value: unknown): JsonObject | undefined {
  if (!isJsonObject(value)) return undefined;
  const rawUrls = typeof value.urls === "string"
    ? [value.urls]
    : Array.isArray(value.urls)
      ? value.urls.filter((entry): entry is string => typeof entry === "string")
      : [];
  const urls = rawUrls
    .map((entry) => entry.trim())
    .filter((entry) => /^(?:stun|turn|turns):/i.test(entry))
    .filter((entry) => !/:53(?:\?|$)/i.test(entry))
    .slice(0, 16);
  if (urls.length === 0) return undefined;
  const server: JsonObject = { urls };
  const username = getString(value, "username");
  const credential = getString(value, "credential");
  if (username && credential) {
    server.username = username;
    server.credential = credential;
  }
  return server;
}

async function generateTurnIceServers(env: WorkerEnv): Promise<Response> {
  if (!turnConfigured(env)) {
    return apiError(503, "turn_not_configured", "TURN credentials are not configured");
  }
  const ttl = Math.min(172800, Math.max(300, Number(env.TURN_TTL_SECONDS || "21600") || 21600));
  const base = (env.TURN_API_BASE || "https://rtc.live.cloudflare.com/v1/turn").replace(/\/+$/, "");
  const response = await fetch(
    `${base}/keys/${encodeURIComponent(env.TURN_KEY_ID)}/credentials/generate-ice-servers`,
    {
      method: "POST",
      headers: {
        Accept: "application/json",
        Authorization: `Bearer ${env.TURN_KEY_API_TOKEN}`,
        "Content-Type": "application/json"
      },
      body: JSON.stringify({ ttl })
    }
  );
  const declaredLength = Number(response.headers.get("Content-Length") || "0");
  if (declaredLength > 256 * 1024) {
    structuredLog("turn_api_invalid_response", { status: response.status, reason: "body_too_large" });
    return apiError(502, "turn_invalid_response", "TURN API returned an invalid response");
  }
  const text = await response.text();
  if (text.length > 256 * 1024) {
    structuredLog("turn_api_invalid_response", { status: response.status, reason: "body_too_large" });
    return apiError(502, "turn_invalid_response", "TURN API returned an invalid response");
  }
  let parsed: unknown;
  try {
    parsed = text ? JSON.parse(text) : {};
  } catch {
    parsed = {};
  }
  if (!response.ok || !isJsonObject(parsed)) {
    structuredLog("turn_api_failed", { status: response.status });
    return apiError(502, "turn_api_failed", "TURN credentials could not be generated");
  }
  const rawServers = Array.isArray(parsed.iceServers) ? parsed.iceServers : [];
  const iceServers = rawServers
    .map(sanitizeIceServer)
    .filter((server): server is JsonObject => server !== undefined);
  if (iceServers.length === 0) {
    structuredLog("turn_api_invalid_response", { status: response.status, reason: "ice_servers_missing" });
    return apiError(502, "turn_invalid_response", "TURN API did not return ICE servers");
  }
  structuredLog("turn_credentials_issued", { ttl_seconds: ttl, ice_server_groups: iceServers.length });
  return noStoreJson({
    iceServers,
    ttl,
    iceTransportPolicy: "all",
    relayOnly: false
  });
}

async function directParticipantOperation(
  request: Request,
  env: WorkerEnv,
  roomId: string,
  participantId: string,
  suffix: string
): Promise<Response> {
  if (!ROOM_ID_PATTERN.test(roomId) || !PARTICIPANT_ID_PATTERN.test(participantId)) {
    return apiError(400, "invalid_identity", "Room or participant ID is invalid");
  }
  const stub = directRoomStub(env, roomId);
  const authenticated = await directParticipantView(stub, participantId, bearerToken(request));
  if (authenticated instanceof Response) return authenticated;

  if (request.method === "POST" && suffix === "/ice-servers") {
    return generateTurnIceServers(env);
  }
  if (request.method === "DELETE" && suffix === "") {
    return internalRequest(
      stub,
      "/internal/participant",
      "DELETE",
      undefined,
      participantId,
      authenticated.tokenHash
    );
  }
  return apiError(404, "not_found", "Direct participant operation was not found");
}

async function createRealtimeSession(
  env: WorkerEnv,
  stub: DurableObjectStub<RealtimeRoom>,
  participantId: string,
  tokenHash: string,
  view: ParticipantView
): Promise<Response> {
  if (view.session_id) {
    return noStoreJson({ sessionId: view.session_id, reused: true });
  }
  const created = await callRealtime(
    env,
    `/sessions/new?correlationId=${encodeURIComponent(participantId)}`,
    "POST"
  );
  if (created.status !== 201) return realtimeResponse(created);
  const sessionId = getString(created.body, "sessionId");
  if (!sessionId) {
    return apiError(502, "realtime_session_missing", "Realtime API did not return a session ID");
  }
  const saved = await internalRequest(
    stub,
    "/internal/session",
    "PUT",
    { session_id: sessionId },
    participantId,
    tokenHash
  );
  if (!saved.ok) {
    structuredLog("realtime_session_save_failed", { participant_id: participantId });
    return apiError(502, "session_coordination_failed", "Realtime session could not be registered with the room");
  }
  return realtimeResponse(created);
}

async function participantOperation(
  request: Request,
  env: WorkerEnv,
  roomId: string,
  participantId: string,
  suffix: string
): Promise<Response> {
  if (!ROOM_ID_PATTERN.test(roomId) || !PARTICIPANT_ID_PATTERN.test(participantId)) {
    return apiError(400, "invalid_identity", "Room or participant ID is invalid");
  }
  const token = bearerToken(request);
  const stub = roomStub(env, roomId);
  const authenticated = await participantView(stub, participantId, token);
  if (authenticated instanceof Response) return authenticated;
  const { view, tokenHash } = authenticated;

  if (request.method === "POST" && suffix === "/session") {
    return createRealtimeSession(env, stub, participantId, tokenHash, view);
  }
  if (!view.session_id) {
    return apiError(409, "session_required", "Create the participant Realtime session first");
  }
  const sessionPath = `/sessions/${encodeURIComponent(view.session_id)}`;

  if (request.method === "POST" && suffix === "/ready") {
    return internalRequest(stub, "/internal/ready", "POST", {}, participantId, tokenHash);
  }

  if (request.method === "POST" && suffix === "/realtime/establish") {
    return realtimeResponse(await callRealtime(env, `${sessionPath}/datachannels/establish`, "POST", {
      dataChannel: { location: "remote", dataChannelName: "server-events" }
    }));
  }

  if (request.method === "PUT" && suffix === "/realtime/renegotiate") {
    const body = await readJsonObject(request);
    const description = body.sessionDescription;
    const sdp = isJsonObject(description) && typeof description.sdp === "string"
      ? description.sdp
      : "";
    if (!isJsonObject(description)
      || getString(description, "type") !== "answer"
      || sdp.length < 32) {
      return apiError(400, "invalid_answer", "A WebRTC answer sessionDescription is required");
    }
    let normalizedSdp = sdp.replaceAll("\r\n", "\n").replaceAll("\r", "\n").replaceAll("\n", "\r\n");
    if (!normalizedSdp.endsWith("\r\n")) normalizedSdp += "\r\n";
    return realtimeResponse(await callRealtime(env, `${sessionPath}/renegotiate`, "PUT", {
      sessionDescription: { type: "answer", sdp: normalizedSdp }
    }));
  }

  if (request.method === "POST" && suffix === "/realtime/publish") {
    if (!view.is_host) return apiError(403, "host_required", "Only the listen server can publish client channels");
    const body = await readJsonObject(request);
    const peerId = getString(body, "peer_id");
    const peer = view.participants.find((candidate) => candidate.participant_id === peerId && !candidate.is_host);
    if (!peer?.session_id || !peer.realtime_ready) {
      return apiError(409, "peer_not_ready", "Client Realtime transport is not ready");
    }
    const result = await callRealtime(env, `${sessionPath}/datachannels/new`, "POST", {
      dataChannels: [{ location: "local", dataChannelName: `ue-down-${peerId}` }]
    });
    logDataChannelResult("publish_downlink", result);
    return realtimeResponse(result);
  }

  if (request.method === "POST" && suffix === "/realtime/subscribe") {
    if (view.is_host) return apiError(403, "client_required", "The listen server cannot subscribe as a client");
    if (!view.host_session_id) return apiError(409, "host_not_ready", "Host Realtime session is not ready");
    const result = await callRealtime(env, `${sessionPath}/datachannels/new`, "POST", {
      dataChannels: [{
        location: "remote",
        sessionId: view.host_session_id,
        dataChannelName: `ue-down-${participantId}`
      }]
    });
    logDataChannelResult("subscribe_downlink", result);
    return realtimeResponse(result);
  }

  if (request.method === "POST" && suffix === "/realtime/publish-upstream") {
    if (view.is_host) return apiError(403, "client_required", "Only clients publish uplink channels");
    const result = await callRealtime(env, `${sessionPath}/datachannels/new`, "POST", {
      dataChannels: [{ location: "local", dataChannelName: `ue-up-${participantId}` }]
    });
    logDataChannelResult("publish_uplink", result);
    return realtimeResponse(result);
  }

  if (request.method === "POST" && suffix === "/realtime/subscribe-upstream") {
    if (!view.is_host) return apiError(403, "host_required", "Only the listen server subscribes to uplinks");
    const body = await readJsonObject(request);
    const peerId = getString(body, "peer_id");
    const peer = view.participants.find((candidate) => candidate.participant_id === peerId && !candidate.is_host);
    if (!peer?.session_id || !peer.realtime_ready) {
      return apiError(409, "peer_not_ready", "Client Realtime transport is not ready");
    }
    const result = await callRealtime(env, `${sessionPath}/datachannels/new`, "POST", {
      dataChannels: [{
        location: "remote",
        sessionId: peer.session_id,
        dataChannelName: `ue-up-${peerId}`
      }]
    });
    logDataChannelResult("subscribe_uplink", result);
    return realtimeResponse(result);
  }

  if (request.method === "DELETE" && suffix === "") {
    return internalRequest(
      stub, "/internal/participant", "DELETE", undefined, participantId, tokenHash
    );
  }

  return apiError(404, "not_found", "Participant operation was not found");
}

async function handle(request: Request, env: WorkerEnv): Promise<Response> {
  const url = new URL(request.url);
  const requestId = crypto.randomUUID();
  if (request.method === "GET" && url.pathname === "/health") {
    const hybridConfigured = flowConfigured(env) && directConfigured(env);
    return noStoreJson({
      ok: true,
      service: "webrtc4unreal-realtime",
      protocol: "cloudflare-realtime.v1",
      direct_protocol: "cloudflare-direct.v1",
      hybrid_protocol: "flow-cloudflare-fallback.v1",
      default_provider: "FlowCloudflareFallback",
      configured: sfuConfigured(env) || directConfigured(env) || flowConfigured(env),
      sfu_configured: sfuConfigured(env),
      direct_configured: directConfigured(env),
      flow_configured: flowConfigured(env),
      hybrid_configured: hybridConfigured,
      turn_configured: turnConfigured(env),
      durable_objects: true
    });
  }
  if (request.method === "GET" && url.pathname === "/api/p2p/sessions/host") {
    if (!await bootstrapAuthorized(request, env)) {
      return apiError(401, "bootstrap_unauthorized", "Client bootstrap key is invalid");
    }
    return createFlowHostSession(request, env);
  }
  if (request.method === "GET" && url.pathname === "/api/p2p/sessions/join") {
    if (!await bootstrapAuthorized(request, env)) {
      return apiError(401, "bootstrap_unauthorized", "Client bootstrap key is invalid");
    }
    return createFlowJoinSession(request, env);
  }
  if (request.method === "POST" && url.pathname === "/v1/p2p/rooms") {
    return createDirectRoom(request, env, url.origin);
  }
  const directJoinMatch = /^\/v1\/p2p\/rooms\/([^/]+)\/join$/.exec(url.pathname);
  if (request.method === "POST" && directJoinMatch?.[1]) {
    return joinDirectRoom(request, env, directJoinMatch[1], url.origin);
  }
  const directConnectMatch = /^\/v1\/p2p\/rooms\/([^/]+)\/connect$/.exec(url.pathname);
  if (request.method === "GET" && directConnectMatch?.[1]) {
    return connectDirectRoom(request, env, directConnectMatch[1]);
  }
  const directParticipantMatch =
    /^\/v1\/p2p\/rooms\/([^/]+)\/participants\/([^/]+)(\/.*)?$/.exec(url.pathname);
  if (directParticipantMatch?.[1] && directParticipantMatch[2]) {
    return directParticipantOperation(
      request,
      env,
      directParticipantMatch[1],
      directParticipantMatch[2],
      directParticipantMatch[3] ?? ""
    );
  }
  if (request.method === "POST" && url.pathname === "/v1/rooms") {
    return createRoom(request, env, url.origin);
  }
  const joinMatch = /^\/v1\/rooms\/([^/]+)\/join$/.exec(url.pathname);
  if (request.method === "POST" && joinMatch?.[1]) {
    return joinRoom(request, env, joinMatch[1], url.origin);
  }
  const connectMatch = /^\/v1\/rooms\/([^/]+)\/connect$/.exec(url.pathname);
  if (request.method === "GET" && connectMatch?.[1]) {
    return connectRoom(request, env, connectMatch[1]);
  }
  const participantMatch = /^\/v1\/rooms\/([^/]+)\/participants\/([^/]+)(\/.*)?$/.exec(url.pathname);
  if (participantMatch?.[1] && participantMatch[2]) {
    return participantOperation(
      request,
      env,
      participantMatch[1],
      participantMatch[2],
      participantMatch[3] ?? ""
    );
  }
  structuredLog("request_not_found", { request_id: requestId, method: request.method, path: url.pathname });
  return apiError(404, "not_found", "Endpoint was not found");
}

export default {
  async fetch(request: Request, env: WorkerEnv): Promise<Response> {
    try {
      return await handle(request, env);
    } catch (error) {
      const message = error instanceof Error ? error.message : "unknown_worker_error";
      structuredLog("worker_request_failed", {
        method: request.method,
        path: new URL(request.url).pathname,
        message: message.split(":", 1)[0]
      });
      if (message === "request_too_large") {
        return apiError(413, message, "Request body is too large");
      }
      if (message === "json_object_required" || error instanceof SyntaxError) {
        return apiError(400, "invalid_json", "A JSON object is required");
      }
      if (message.startsWith("room_http_404")) {
        return apiError(404, "room_not_found", "Room does not exist or has expired");
      }
      if (message.startsWith("room_http_409")) {
        return apiError(409, "room_conflict", message.slice(message.indexOf(":") + 1));
      }
      return apiError(500, "internal_error", "WebRTC coordination failed");
    }
  }
} satisfies ExportedHandler<WorkerEnv>;
