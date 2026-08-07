import { DurableObject } from "cloudflare:workers";

import {
  apiError,
  constantTimeEqual,
  getInteger,
  getString,
  isJsonObject,
  noStoreJson,
  readJsonObject,
  structuredLog
} from "./security";
import type { DirectParticipantView, JsonObject, WorkerEnv } from "./types";

interface DirectRoomRow {
  [key: string]: SqlStorageValue;
  room_id: string;
  room_name: string;
  host_id: string;
  max_participants: number;
  created_at: number;
}

interface DirectParticipantRow {
  [key: string]: SqlStorageValue;
  participant_id: string;
  token_hash: string;
  is_host: number;
  joined_at: number;
  last_seen: number;
}

interface DirectSocketAttachment {
  participantId: string;
}

const MAX_CONTROL_BYTES = 256 * 1024;
const MAX_SDP_CHARS = 192 * 1024;
const MAX_CANDIDATE_CHARS = 8 * 1024;
const textEncoder = new TextEncoder();

function firstRow<T extends Record<string, SqlStorageValue>>(cursor: SqlStorageCursor<T>): T | undefined {
  for (const row of cursor) return row;
  return undefined;
}

export class DirectRoom extends DurableObject<WorkerEnv> {
  constructor(ctx: DurableObjectState, env: WorkerEnv) {
    super(ctx, env);
    this.ctx.storage.sql.exec(`
      CREATE TABLE IF NOT EXISTS room (
        singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
        room_id TEXT NOT NULL,
        room_name TEXT NOT NULL,
        host_id TEXT NOT NULL,
        max_participants INTEGER NOT NULL,
        created_at INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS participants (
        participant_id TEXT PRIMARY KEY,
        token_hash TEXT NOT NULL,
        is_host INTEGER NOT NULL,
        joined_at INTEGER NOT NULL,
        last_seen INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_direct_participants_host ON participants(is_host);
    `);
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    try {
      if (request.method === "POST" && url.pathname === "/internal/create") {
        return await this.createRoom(request);
      }
      if (request.method === "POST" && url.pathname === "/internal/join") {
        return await this.joinRoom(request);
      }
      if (request.method === "GET" && url.pathname === "/internal/participant") {
        return this.getAuthenticatedParticipant(request);
      }
      if (request.method === "GET" && url.pathname === "/internal/info") {
        return this.getRoomInfo();
      }
      if (request.method === "GET" && url.pathname === "/internal/connect") {
        return this.connectWebSocket(request);
      }
      if (request.method === "DELETE" && url.pathname === "/internal/participant") {
        return this.removeParticipant(request);
      }
      return apiError(404, "not_found", "Direct room operation was not found");
    } catch (error) {
      const message = error instanceof Error ? error.message : "unknown_direct_room_error";
      structuredLog("direct_room_request_failed", { message, path: url.pathname });
      if (message === "request_too_large") {
        return apiError(413, message, "Request body is too large");
      }
      if (message === "json_object_required" || error instanceof SyntaxError) {
        return apiError(400, "invalid_json", "A JSON object is required");
      }
      return apiError(500, "direct_room_internal_error", "Direct room coordination failed");
    }
  }

  async alarm(): Promise<void> {
    for (const socket of this.ctx.getWebSockets()) {
      try {
        socket.close(1001, "Room expired");
      } catch {
        // The peer may already have closed while the alarm was running.
      }
    }
    await this.ctx.storage.deleteAll();
    structuredLog("direct_room_expired");
  }

  webSocketMessage(socket: WebSocket, message: string | ArrayBuffer): void {
    const attachment = socket.deserializeAttachment() as DirectSocketAttachment | null;
    if (!attachment?.participantId) {
      socket.close(1008, "Missing participant attachment");
      return;
    }
    const text = typeof message === "string" ? message : new TextDecoder().decode(message);
    if (textEncoder.encode(text).byteLength > MAX_CONTROL_BYTES) {
      socket.close(1009, "Control message too large");
      return;
    }
    let frame: unknown;
    try {
      frame = JSON.parse(text);
    } catch {
      this.sendSocket(socket, { type: "error", code: "invalid_json" });
      return;
    }
    if (!isJsonObject(frame)) {
      this.sendSocket(socket, { type: "error", code: "invalid_frame" });
      return;
    }

    const sender = this.findParticipant(attachment.participantId);
    if (!sender) {
      socket.close(1008, "Participant no longer exists");
      return;
    }
    this.ctx.storage.sql.exec(
      "UPDATE participants SET last_seen = ? WHERE participant_id = ?",
      Date.now(), sender.participant_id
    );

    const type = getString(frame, "type");
    if (type === "ping") {
      this.sendSocket(socket, { type: "pong", timestamp: Date.now() });
      return;
    }
    if (type === "p2p_offer") {
      this.forwardOffer(sender, frame, socket);
      return;
    }
    if (type === "p2p_answer") {
      this.forwardAnswer(sender, frame, socket);
      return;
    }
    if (type === "p2p_ice") {
      this.forwardIceCandidate(sender, frame, socket);
      return;
    }
    if (type === "close_peer") {
      this.forwardPeerClose(sender, frame, socket);
      return;
    }
    this.sendSocket(socket, { type: "error", code: "unsupported_frame" });
  }

  webSocketClose(socket: WebSocket, code: number, reason: string, wasClean: boolean): void {
    const attachment = socket.deserializeAttachment() as DirectSocketAttachment | null;
    if (attachment?.participantId) {
      this.broadcast({
        type: "peer_disconnected",
        peer_id: attachment.participantId,
        code,
        clean: wasClean,
        reason: reason.slice(0, 128)
      }, attachment.participantId);
    }
  }

  webSocketError(socket: WebSocket): void {
    const attachment = socket.deserializeAttachment() as DirectSocketAttachment | null;
    if (attachment?.participantId) {
      this.broadcast({ type: "peer_disconnected", peer_id: attachment.participantId }, attachment.participantId);
    }
  }

  private async createRoom(request: Request): Promise<Response> {
    if (this.getRoom()) return apiError(409, "room_exists", "Room already exists");
    const body = await readJsonObject(request);
    const roomId = getString(body, "room_id");
    const roomName = getString(body, "room_name").slice(0, 128) || "Unreal Direct P2P Room";
    const hostId = getString(body, "host_id");
    const hostTokenHash = getString(body, "host_token_hash");
    const maxParticipants = Math.min(100, Math.max(2, getInteger(body, "max_participants", 4)));
    const ttlSeconds = Math.min(86400, Math.max(300, getInteger(body, "ttl_seconds", 21600)));
    if (!roomId || !hostId || hostTokenHash.length !== 64) {
      return apiError(400, "invalid_room", "Room identity is invalid");
    }
    const now = Date.now();
    this.ctx.storage.sql.exec(
      "INSERT INTO room(singleton, room_id, room_name, host_id, max_participants, created_at) VALUES(1, ?, ?, ?, ?, ?)",
      roomId, roomName, hostId, maxParticipants, now
    );
    this.ctx.storage.sql.exec(
      "INSERT INTO participants(participant_id, token_hash, is_host, joined_at, last_seen) VALUES(?, ?, 1, ?, ?)",
      hostId, hostTokenHash, now, now
    );
    await this.ctx.storage.setAlarm(now + ttlSeconds * 1000);
    structuredLog("direct_room_created", { room_id: roomId, max_participants: maxParticipants });
    return noStoreJson({ room_id: roomId, room_name: roomName, host_id: hostId, max_participants: maxParticipants }, 201);
  }

  private async joinRoom(request: Request): Promise<Response> {
    const room = this.getRoom();
    if (!room) return apiError(404, "room_not_found", "Room does not exist or has expired");
    const countRow = firstRow(this.ctx.storage.sql.exec<{ count: number }>("SELECT COUNT(*) AS count FROM participants"));
    if ((countRow?.count ?? 0) >= room.max_participants) {
      return apiError(409, "room_full", "Room has reached its participant limit");
    }
    const body = await readJsonObject(request);
    const participantId = getString(body, "participant_id");
    const tokenHash = getString(body, "token_hash");
    if (!participantId || tokenHash.length !== 64) {
      return apiError(400, "invalid_participant", "Participant identity is invalid");
    }
    const now = Date.now();
    this.ctx.storage.sql.exec(
      "INSERT INTO participants(participant_id, token_hash, is_host, joined_at, last_seen) VALUES(?, ?, 0, ?, ?)",
      participantId, tokenHash, now, now
    );
    structuredLog("direct_participant_joined", { room_id: room.room_id, participant_id: participantId });
    return noStoreJson({
      room_id: room.room_id,
      room_name: room.room_name,
      host_id: room.host_id,
      max_participants: room.max_participants
    }, 201);
  }

  private getAuthenticatedParticipant(request: Request): Response {
    const participant = this.authenticateParticipant(request);
    if (!participant) return apiError(401, "participant_unauthorized", "Participant token is invalid");
    return noStoreJson(this.makeParticipantView(participant));
  }

  private getRoomInfo(): Response {
    const room = this.getRoom();
    if (!room) return apiError(404, "room_not_found", "Room does not exist or has expired");
    return noStoreJson({
      room_id: room.room_id,
      room_name: room.room_name,
      host_id: room.host_id,
      max_participants: room.max_participants
    });
  }

  private connectWebSocket(request: Request): Response {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return apiError(426, "websocket_required", "Upgrade to WebSocket is required");
    }
    const participant = this.authenticateParticipant(request);
    if (!participant) return apiError(401, "participant_unauthorized", "Participant token is invalid");
    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    server.serializeAttachment({ participantId: participant.participant_id } satisfies DirectSocketAttachment);
    this.ctx.acceptWebSocket(server, [participant.participant_id]);
    structuredLog("direct_control_socket_connected", {
      participant_id: participant.participant_id,
      role: participant.is_host === 1 ? "host" : "client",
      room_sockets: this.ctx.getWebSockets().length
    });
    this.sendSocket(server, { type: "connected", ...this.makeParticipantView(participant) });
    this.broadcast({
      type: "participant_connected",
      peer_id: participant.participant_id
    }, participant.participant_id);
    return new Response(null, { status: 101, webSocket: client });
  }

  private removeParticipant(request: Request): Response {
    const participant = this.authenticateParticipant(request);
    if (!participant) return apiError(401, "participant_unauthorized", "Participant token is invalid");
    const room = this.getRoom();
    this.ctx.storage.sql.exec("DELETE FROM participants WHERE participant_id = ?", participant.participant_id);
    for (const socket of this.ctx.getWebSockets(participant.participant_id)) {
      try {
        socket.close(1000, "Participant left");
      } catch {
        // Socket already closed.
      }
    }
    this.broadcast({ type: "peer_left", peer_id: participant.participant_id }, participant.participant_id);
    if (participant.is_host === 1) {
      for (const socket of this.ctx.getWebSockets()) {
        try {
          socket.close(1001, "Host left");
        } catch {
          // Socket already closed.
        }
      }
      this.ctx.storage.sql.exec("DELETE FROM participants");
      this.ctx.storage.sql.exec("DELETE FROM room");
    }
    structuredLog("direct_participant_removed", {
      room_id: room?.room_id ?? "",
      participant_id: participant.participant_id,
      role: participant.is_host === 1 ? "host" : "client"
    });
    return new Response(null, { status: 204 });
  }

  private forwardOffer(sender: DirectParticipantRow, frame: JsonObject, socket: WebSocket): void {
    if (sender.is_host !== 1) {
      this.sendSocket(socket, { type: "error", code: "host_required" });
      return;
    }
    const targetId = getString(frame, "target_peer_id");
    const target = this.findParticipant(targetId);
    // SDP is line-oriented and its final CRLF is significant to libwebrtc.
    // Do not use getString(), which intentionally trims ordinary control fields.
    const sdp = typeof frame.sdp === "string" ? frame.sdp : "";
    if (!target || target.is_host === 1) {
      this.sendSocket(socket, { type: "error", code: "target_not_found", peer_id: targetId });
      return;
    }
    if (sdp.length < 32 || sdp.length > MAX_SDP_CHARS) {
      this.sendSocket(socket, { type: "error", code: "invalid_offer" });
      return;
    }
    this.sendToParticipant(targetId, { type: "p2p_offer", peer_id: sender.participant_id, sdp });
  }

  private forwardAnswer(sender: DirectParticipantRow, frame: JsonObject, socket: WebSocket): void {
    const room = this.getRoom();
    if (!room || sender.is_host === 1) {
      this.sendSocket(socket, { type: "error", code: "client_required" });
      return;
    }
    const targetId = getString(frame, "target_peer_id") || room.host_id;
    const sdp = typeof frame.sdp === "string" ? frame.sdp : "";
    if (targetId !== room.host_id || sdp.length < 32 || sdp.length > MAX_SDP_CHARS) {
      this.sendSocket(socket, { type: "error", code: "invalid_answer" });
      return;
    }
    this.sendToParticipant(room.host_id, { type: "p2p_answer", peer_id: sender.participant_id, sdp });
  }

  private forwardIceCandidate(sender: DirectParticipantRow, frame: JsonObject, socket: WebSocket): void {
    const room = this.getRoom();
    if (!room) return;
    const targetId = getString(frame, "target_peer_id");
    const target = this.findParticipant(targetId);
    const candidate = getString(frame, "candidate");
    const sdpMid = getString(frame, "sdp_mid").slice(0, 256);
    const sdpMLineIndex = getInteger(frame, "sdp_mline_index", -1);
    const validRoute = sender.is_host === 1
      ? Boolean(target && target.is_host !== 1)
      : targetId === room.host_id;
    if (!validRoute || candidate.length < 8 || candidate.length > MAX_CANDIDATE_CHARS
      || sdpMLineIndex < 0 || sdpMLineIndex > 64) {
      this.sendSocket(socket, { type: "error", code: "invalid_ice_candidate", peer_id: targetId });
      return;
    }
    this.sendToParticipant(targetId, {
      type: "p2p_ice",
      peer_id: sender.participant_id,
      candidate,
      sdp_mid: sdpMid,
      sdp_mline_index: sdpMLineIndex
    });
  }

  private forwardPeerClose(sender: DirectParticipantRow, frame: JsonObject, socket: WebSocket): void {
    if (sender.is_host !== 1) {
      this.sendSocket(socket, { type: "error", code: "host_required" });
      return;
    }
    const targetId = getString(frame, "target_peer_id");
    const target = this.findParticipant(targetId);
    if (!target || target.is_host === 1) {
      this.sendSocket(socket, { type: "error", code: "target_not_found" });
      return;
    }
    this.sendToParticipant(targetId, { type: "peer_closed", peer_id: sender.participant_id });
  }

  private getRoom(): DirectRoomRow | undefined {
    return firstRow(this.ctx.storage.sql.exec<DirectRoomRow>(
      "SELECT room_id, room_name, host_id, max_participants, created_at FROM room WHERE singleton = 1"
    ));
  }

  private findParticipant(participantId: string): DirectParticipantRow | undefined {
    if (!participantId) return undefined;
    return firstRow(this.ctx.storage.sql.exec<DirectParticipantRow>(
      "SELECT participant_id, token_hash, is_host, joined_at, last_seen FROM participants WHERE participant_id = ?",
      participantId
    ));
  }

  private authenticateParticipant(request: Request): DirectParticipantRow | undefined {
    const participantId = request.headers.get("X-Participant-ID")?.trim() ?? "";
    const presentedHash = request.headers.get("X-Participant-Hash")?.trim() ?? "";
    const participant = this.findParticipant(participantId);
    return participant && constantTimeEqual(participant.token_hash, presentedHash) ? participant : undefined;
  }

  private makeParticipantView(participant: DirectParticipantRow): DirectParticipantView {
    const room = this.getRoom();
    if (!room) throw new Error("room_missing");
    const participants = Array.from(this.ctx.storage.sql.exec<DirectParticipantRow>(
      "SELECT participant_id, token_hash, is_host, joined_at, last_seen FROM participants ORDER BY joined_at"
    )).map((entry) => ({
      participant_id: entry.participant_id,
      is_host: entry.is_host === 1,
      connected: this.ctx.getWebSockets(entry.participant_id).length > 0
    }));
    return {
      participant_id: participant.participant_id,
      is_host: participant.is_host === 1,
      host_id: room.host_id,
      room_name: room.room_name,
      max_participants: room.max_participants,
      participants
    };
  }

  private sendSocket(socket: WebSocket, frame: JsonObject): boolean {
    const frameType = getString(frame, "type") || "unknown";
    try {
      socket.send(JSON.stringify(frame));
      return true;
    } catch (error) {
      structuredLog("direct_control_socket_send_failed", {
        frame_type: frameType,
        ready_state: socket.readyState,
        message: error instanceof Error ? error.message.slice(0, 256) : "unknown_send_error"
      });
      return false;
    }
  }

  private sendToParticipant(participantId: string, frame: JsonObject): void {
    const sockets = this.ctx.getWebSockets(participantId);
    let delivered = 0;
    for (const socket of sockets) {
      if (this.sendSocket(socket, frame)) delivered += 1;
    }
    structuredLog("direct_control_frame_delivery", {
      frame_type: getString(frame, "type") || "unknown",
      target_participant_id: participantId,
      matched_sockets: sockets.length,
      delivered_sockets: delivered
    });
  }

  private broadcast(frame: JsonObject, excludedParticipantId = ""): void {
    for (const socket of this.ctx.getWebSockets()) {
      const attachment = socket.deserializeAttachment() as DirectSocketAttachment | null;
      if (excludedParticipantId && attachment?.participantId === excludedParticipantId) continue;
      this.sendSocket(socket, frame);
    }
  }
}
