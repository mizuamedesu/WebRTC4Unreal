import {
  apiError,
  getInteger,
  getString,
  isJsonObject,
  noStoreJson,
  structuredLog
} from "./security";
import type { JsonObject, WorkerEnv } from "./types";

const FLOW_ROOM_ID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const PARTICIPANT_ID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const MAX_UPSTREAM_BYTES = 512 * 1024;
const HOST_PERMISSIONS = [
  "flow.room.create",
  "flow.room.join",
  "flow.signal.connect",
  "flow.turn.issue"
];
const JOIN_PERMISSIONS = [
  "flow.room.join",
  "flow.signal.connect",
  "flow.turn.issue"
];

interface FlowCredential {
  contextId: string;
  role: "host" | "join";
  principalId: string;
  principal: string;
  timestamp: string;
  signature: string;
  expiresInSeconds: number;
}

interface FlowConnection {
  signalingUrls: string[];
  protocol: string;
  asyncApiUrls: string[];
  iceServers: JsonObject[];
  iceExpiresAt: string;
}

interface DirectRoomInfo {
  room_id: string;
  room_name: string;
  host_id: string;
  max_participants: number;
}

export function flowConfigured(env: WorkerEnv): boolean {
  return Boolean(
    env.CLIENT_ACCESS_KEY
    && env.HCF_DEVELOPER_CREDENTIAL
    && env.FLOW_API_BASE_URL
    && env.HCF_CREDENTIAL_ENDPOINT
  );
}

function clampInteger(value: string | undefined, fallback: number, min: number, max: number): number {
  const parsed = Number.parseInt(value ?? "", 10);
  return Number.isFinite(parsed) ? Math.min(max, Math.max(min, parsed)) : fallback;
}

function flowSafeName(value: string, fallback: string, maxLength: number): string {
  // Flow 0.1.13 rejects spaces and punctuation even though its OpenAPI schema
  // currently only advertises maxLength. Keep the normalization server-side so
  // room labels from Unreal never make primary-path preparation fail.
  const normalized = value.normalize("NFKC").replace(/[^A-Za-z0-9]/g, "").slice(0, maxLength);
  return normalized || fallback;
}

function requestedParticipantId(request: Request): string | Response {
  const requested = request.headers.get("X-WebRTC4Unreal-Participant-Id")?.trim() ?? "";
  if (!requested) return crypto.randomUUID();
  if (!PARTICIPANT_ID_PATTERN.test(requested)) {
    return apiError(400, "invalid_participant_id", "Requested participant ID must be a UUID");
  }
  return requested;
}

async function readBoundedJson(response: Response, source: string): Promise<JsonObject> {
  const declaredLength = Number(response.headers.get("Content-Length") ?? "0");
  if (Number.isFinite(declaredLength) && declaredLength > MAX_UPSTREAM_BYTES) {
    throw new Error(`${source}_response_too_large`);
  }
  const text = await response.text();
  if (new TextEncoder().encode(text).byteLength > MAX_UPSTREAM_BYTES) {
    throw new Error(`${source}_response_too_large`);
  }
  if (!text) return {};
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch {
    throw new Error(`${source}_invalid_json`);
  }
  if (!isJsonObject(parsed)) throw new Error(`${source}_invalid_response`);
  return parsed;
}

function caseInsensitiveString(object: JsonObject, targetName: string): string {
  const target = targetName.toLowerCase();
  for (const [name, value] of Object.entries(object)) {
    if (name.toLowerCase() === target && typeof value === "string") return value.trim();
  }
  return "";
}

function upstreamMessage(payload: JsonObject, fallback: string): string {
  const nested = isJsonObject(payload.error) ? payload.error : {};
  return (getString(nested, "message") || getString(payload, "message") || fallback).slice(0, 512);
}

async function issueFlowCredential(
  env: WorkerEnv,
  role: "host" | "join",
  principalId: string
): Promise<FlowCredential> {
  const expiresInSeconds = clampInteger(env.FLOW_TOKEN_TTL_SECONDS, 300, 60, 900);
  const response = await fetch(env.HCF_CREDENTIAL_ENDPOINT.replace(/\/+$/, ""), {
    method: "POST",
    headers: {
      Accept: "application/json",
      Authorization: `Bearer ${env.HCF_DEVELOPER_CREDENTIAL}`,
      "Content-Type": "application/json"
    },
    body: JSON.stringify({
      principal_id: principalId,
      permissions: role === "host" ? HOST_PERMISSIONS : JOIN_PERMISSIONS,
      expires_in_seconds: expiresInSeconds
    }),
    signal: AbortSignal.timeout(10_000)
  });
  const payload = await readBoundedJson(response, "credential_service");
  if (!response.ok) {
    throw new Error(`credential_http_${response.status}:${upstreamMessage(payload, "Credential issuance failed")}`);
  }
  const returnedHeaders = isJsonObject(payload.headers) ? payload.headers : {};
  const contextId = getString(payload, "context_id");
  const principal = caseInsensitiveString(returnedHeaders, "x-flow-principal");
  const timestamp = caseInsensitiveString(returnedHeaders, "x-flow-timestamp");
  const signature = caseInsensitiveString(returnedHeaders, "x-flow-signature");
  if (!contextId || !principal || !timestamp || !signature) {
    throw new Error("credential_service_incomplete_response");
  }
  return {
    contextId,
    role,
    principalId,
    principal,
    timestamp,
    signature,
    expiresInSeconds
  };
}

async function revokeFlowCredential(env: WorkerEnv, contextId: string): Promise<void> {
  const endpoint = `${env.HCF_CREDENTIAL_ENDPOINT.replace(/\/+$/, "")}/${encodeURIComponent(contextId)}`;
  const response = await fetch(endpoint, {
    method: "DELETE",
    headers: {
      Accept: "application/json",
      Authorization: `Bearer ${env.HCF_DEVELOPER_CREDENTIAL}`
    },
    signal: AbortSignal.timeout(10_000)
  });
  if (!response.ok && response.status !== 404) {
    throw new Error(`credential_revoke_http_${response.status}`);
  }
  await response.body?.cancel();
}

function flowHeaders(credential: FlowCredential): Headers {
  return new Headers({
    Accept: "application/json",
    "Content-Type": "application/json",
    "X-Flow-Principal": credential.principal,
    "X-Flow-Timestamp": credential.timestamp,
    "X-Flow-Signature": credential.signature
  });
}

async function flowRequest(
  env: WorkerEnv,
  credential: FlowCredential,
  path: string,
  method: "GET" | "POST",
  body?: JsonObject
): Promise<JsonObject> {
  const base = env.FLOW_API_BASE_URL.replace(/\/+$/, "");
  const init: RequestInit = {
    method,
    headers: flowHeaders(credential),
    signal: AbortSignal.timeout(10_000)
  };
  if (body) init.body = JSON.stringify(body);
  const response = await fetch(`${base}${path}`, init);
  const payload = await readBoundedJson(response, "flow");
  if (!response.ok) {
    throw new Error(`flow_http_${response.status}:${upstreamMessage(payload, "Flow request failed")}`);
  }
  return payload;
}

async function withFlowCredential<T>(
  env: WorkerEnv,
  role: "host" | "join",
  principalId: string,
  operation: (credential: FlowCredential) => Promise<T>
): Promise<T> {
  const credential = await issueFlowCredential(env, role, principalId);
  try {
    return await operation(credential);
  } catch (error) {
    try {
      await revokeFlowCredential(env, credential.contextId);
    } catch (revokeError) {
      structuredLog("flow_rejected_context_revoke_failed", {
        message: revokeError instanceof Error ? revokeError.message.slice(0, 128) : "unknown_revoke_error"
      });
    }
    throw error;
  }
}

function stringArray(value: unknown): string[] {
  return Array.isArray(value)
    ? value.filter((entry): entry is string => typeof entry === "string" && entry.length > 0)
    : [];
}

function requireP2PConnection(joinResponse: JsonObject): FlowConnection {
  const connection = isJsonObject(joinResponse.connection) ? joinResponse.connection : {};
  const signalingUrls = stringArray(connection.urls);
  const asyncApiUrls = stringArray(connection.asyncapi_urls);
  if (getString(connection, "type") !== "p2p"
    || getString(connection, "protocol") !== "flow-signaling.v1"
    || signalingUrls.length === 0
    || signalingUrls.some((url) => !/^wss:\/\//i.test(url))
    || asyncApiUrls.length === 0
    || asyncApiUrls.some((url) => !/^https:\/\//i.test(url))) {
    throw new Error("flow_p2p_signaling_missing");
  }

  const ice = isJsonObject(connection.ice) ? connection.ice : {};
  const expiresAt = getString(ice, "expires_at");
  const sourceServers = Array.isArray(ice.ice_servers) ? ice.ice_servers : [];
  const iceServers: JsonObject[] = [];
  let hasStun = false;
  let hasAuthenticatedTurn = false;
  for (const source of sourceServers) {
    if (!isJsonObject(source)) throw new Error("flow_ice_server_invalid");
    const urls = stringArray(source.urls);
    if (urls.length === 0 || urls.some((url) => !/^(?:stun|stuns|turn|turns):/i.test(url))) {
      throw new Error("flow_ice_url_invalid");
    }
    const username = getString(source, "username");
    const credential = getString(source, "credential");
    hasStun ||= urls.some((url) => /^stuns?:/i.test(url));
    if (urls.some((url) => /^turns?:/i.test(url))) {
      if (!username || !credential) throw new Error("flow_turn_credentials_missing");
      hasAuthenticatedTurn = true;
    }
    iceServers.push({
      urls,
      username: username || null,
      credential: credential || null
    });
  }
  if (!expiresAt || iceServers.length === 0 || !hasStun || !hasAuthenticatedTurn) {
    throw new Error("flow_direct_first_ice_missing");
  }
  return {
    signalingUrls,
    protocol: "flow-signaling.v1",
    asyncApiUrls,
    iceServers,
    iceExpiresAt: expiresAt
  };
}

function sessionPayload(
  room: DirectRoomInfo,
  credential: FlowCredential,
  hostPrincipalId: string,
  connection: FlowConnection
): JsonObject {
  return {
    room_id: room.room_id,
    room_name: room.room_name,
    role: credential.role,
    local_principal_id: credential.principalId,
    host_principal_id: hostPrincipalId,
    max_participants: room.max_participants,
    signal_url: connection.signalingUrls[0],
    signaling_urls: connection.signalingUrls,
    protocol: connection.protocol,
    asyncapi_urls: connection.asyncApiUrls,
    signaling_auth: {
      type: "signed_context",
      principal_context: credential.principal,
      timestamp: credential.timestamp,
      signature: credential.signature,
      expires_in_seconds: credential.expiresInSeconds
    },
    ice_servers: connection.iceServers,
    ice_expires_at: connection.iceExpiresAt,
    ice_policy: "all",
    relay_only: false,
    turn_fallback: true,
    transport: "webrtc-datachannel"
  };
}

async function directRoomInfo(env: WorkerEnv, roomId: string): Promise<DirectRoomInfo> {
  const stub = env.P2P_ROOMS.getByName(roomId);
  const response = await stub.fetch(new Request("https://room.internal/internal/info"));
  const payload: unknown = await response.json();
  if (!response.ok || !isJsonObject(payload)) throw new Error("hybrid_fallback_room_missing");
  const info: DirectRoomInfo = {
    room_id: getString(payload, "room_id"),
    room_name: getString(payload, "room_name"),
    host_id: getString(payload, "host_id"),
    max_participants: getInteger(payload, "max_participants", 0)
  };
  if (info.room_id !== roomId || !PARTICIPANT_ID_PATTERN.test(info.host_id)
    || info.max_participants < 2) {
    throw new Error("hybrid_fallback_room_invalid");
  }
  return info;
}

export async function createFlowHostSession(request: Request, env: WorkerEnv): Promise<Response> {
  if (!flowConfigured(env)) {
    return apiError(503, "flow_not_configured", "Flow developer credentials are not configured");
  }
  const participantId = requestedParticipantId(request);
  if (participantId instanceof Response) return participantId;
  const url = new URL(request.url);
  const roomName = flowSafeName(
    (url.searchParams.get("room_name") ?? "").trim(),
    "UnrealFlowRoom",
    128
  );
  const maxParticipants = clampInteger(url.searchParams.get("max_participants") ?? undefined, 4, 2, 100);

  try {
    const payload = await withFlowCredential(env, "host", participantId, async (credential) => {
      const created = await flowRequest(env, credential, "/v1/rooms", "POST", {
        mode: "p2p",
        name: roomName,
        max_participants: maxParticipants,
        metadata: {
          unreal: {
            transport: "webrtc-datachannel",
            host_principal_id: participantId,
            primary: "flow",
            fallback: "cloudflare-direct",
            ice_policy: "all",
            schema_version: 6
          }
        }
      });
      const roomId = getString(created, "id");
      if (!FLOW_ROOM_ID_PATTERN.test(roomId)) throw new Error("flow_room_id_invalid");
      const room: DirectRoomInfo = {
        room_id: roomId,
        room_name: getString(created, "name") || roomName,
        host_id: participantId,
        max_participants: getInteger(created, "max_participants", maxParticipants)
      };
      const joined = await flowRequest(
        env,
        credential,
        `/v1/rooms/${encodeURIComponent(roomId)}/join`,
        "POST",
        { can_publish: true, can_subscribe: true, display_name: "UnrealHost" }
      );
      return sessionPayload(room, credential, participantId, requireP2PConnection(joined));
    });
    structuredLog("flow_primary_host_ready", { room_id: getString(payload, "room_id") });
    return noStoreJson(payload);
  } catch (error) {
    const message = error instanceof Error ? error.message : "unknown_flow_host_error";
    structuredLog("flow_primary_host_failed", { message: message.slice(0, 256) });
    return apiError(502, "flow_primary_unavailable", "Flow primary session could not be prepared");
  }
}

export async function createFlowJoinSession(request: Request, env: WorkerEnv): Promise<Response> {
  if (!flowConfigured(env)) {
    return apiError(503, "flow_not_configured", "Flow developer credentials are not configured");
  }
  const participantId = requestedParticipantId(request);
  if (participantId instanceof Response) return participantId;
  const url = new URL(request.url);
  const roomId = (url.searchParams.get("room_id") ?? "").trim();
  if (!FLOW_ROOM_ID_PATTERN.test(roomId)) {
    return apiError(400, "invalid_room_id", "A valid room ID is required");
  }

  try {
    const room = await directRoomInfo(env, roomId);
    const payload = await withFlowCredential(env, "join", participantId, async (credential) => {
      const joined = await flowRequest(
        env,
        credential,
        `/v1/rooms/${encodeURIComponent(roomId)}/join`,
        "POST",
        { can_publish: true, can_subscribe: true, display_name: "UnrealClient" }
      );
      if (getString(joined, "room_id") !== roomId || getString(joined, "mode") !== "p2p") {
        throw new Error("flow_join_room_mismatch");
      }
      return sessionPayload(room, credential, room.host_id, requireP2PConnection(joined));
    });
    structuredLog("flow_primary_join_ready", { room_id: roomId, participant_id: participantId });
    return noStoreJson(payload);
  } catch (error) {
    const message = error instanceof Error ? error.message : "unknown_flow_join_error";
    structuredLog("flow_primary_join_failed", { room_id: roomId, message: message.slice(0, 256) });
    return apiError(502, "flow_primary_unavailable", "Flow primary session could not be prepared");
  }
}
