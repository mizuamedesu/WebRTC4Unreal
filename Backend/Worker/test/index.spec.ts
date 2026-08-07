import { SELF } from "cloudflare:test";
import { afterEach, describe, expect, it, vi } from "vitest";

const bootstrapHeaders = {
  "Content-Type": "application/json",
  "X-P2P-Bootstrap-Key": "test-client-key"
};

async function createRoom(maxParticipants = 4): Promise<Record<string, unknown>> {
  const response = await SELF.fetch("https://worker.test/v1/rooms", {
    method: "POST",
    headers: bootstrapHeaders,
    body: JSON.stringify({ room_name: "Worker test", max_participants: maxParticipants })
  });
  expect(response.status).toBe(201);
  return response.json() as Promise<Record<string, unknown>>;
}

async function createDirectRoom(maxParticipants = 4): Promise<Record<string, unknown>> {
  const response = await SELF.fetch("https://worker.test/v1/p2p/rooms", {
    method: "POST",
    headers: bootstrapHeaders,
    body: JSON.stringify({ room_name: "Direct Worker test", max_participants: maxParticipants })
  });
  expect(response.status).toBe(201);
  return response.json() as Promise<Record<string, unknown>>;
}

function receiveFrame(socket: WebSocket): Promise<Record<string, unknown>> {
  return new Promise((resolve, reject) => {
    socket.addEventListener("message", (event) => {
      try {
        resolve(JSON.parse(String(event.data)) as Record<string, unknown>);
      } catch (error) {
        reject(error);
      }
    }, { once: true });
  });
}

afterEach(() => {
  vi.restoreAllMocks();
});

async function connectDirectParticipant(
  session: Record<string, unknown>
): Promise<WebSocket> {
  const testUrl = String(session.signal_url).replace(/^wss:/, "https:").replace(/^ws:/, "http:");
  const response = await SELF.fetch(testUrl, {
    headers: {
      Upgrade: "websocket",
      Authorization: `Bearer ${String(session.participant_token)}`,
      "X-WebRTC4Unreal-Participant": String(session.participant_id)
    }
  });
  expect(response.status).toBe(101);
  if (!response.webSocket) throw new Error("Direct control WebSocket was not returned");
  response.webSocket.accept();
  return response.webSocket;
}

describe("WebRTC4Unreal Realtime Worker", () => {
  it("reports its configuration without exposing credentials", async () => {
    const response = await SELF.fetch("https://worker.test/health");
    expect(response.status).toBe(200);
    const body = await response.json() as Record<string, unknown>;
    expect(body).toMatchObject({
      ok: true,
      configured: true,
      protocol: "cloudflare-realtime.v1",
      direct_protocol: "cloudflare-direct.v1",
      hybrid_protocol: "flow-cloudflare-fallback.v1",
      default_provider: "FlowCloudflareFallback",
      sfu_configured: true,
      direct_configured: true,
      flow_configured: true,
      hybrid_configured: true,
      turn_configured: true,
      durable_objects: true
    });
    expect(JSON.stringify(body)).not.toContain("test-secret");
  });

  it("requires the client bootstrap key", async () => {
    const response = await SELF.fetch("https://worker.test/v1/rooms", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}"
    });
    expect(response.status).toBe(401);
  });

  it("creates and joins a durable room using opaque participant credentials", async () => {
    const room = await createRoom();
    expect(room.protocol).toBe("cloudflare-realtime.v1");
    expect(room.role).toBe("host");
    expect(String(room.participant_token)).toHaveLength(43);

    const response = await SELF.fetch(`https://worker.test/v1/rooms/${room.room_id}/join`, {
      method: "POST",
      headers: bootstrapHeaders,
      body: "{}"
    });
    expect(response.status).toBe(201);
    const participant = await response.json() as Record<string, unknown>;
    expect(participant).toMatchObject({
      room_id: room.room_id,
      host_id: room.host_id,
      role: "client",
      relay_only: true
    });
    expect(participant.participant_id).not.toBe(room.participant_id);
    expect(participant.participant_token).not.toBe(room.participant_token);
  });

  it("enforces the room participant limit", async () => {
    const room = await createRoom(2);
    const firstJoin = await SELF.fetch(`https://worker.test/v1/rooms/${room.room_id}/join`, {
      method: "POST",
      headers: bootstrapHeaders,
      body: "{}"
    });
    expect(firstJoin.status).toBe(201);

    const secondJoin = await SELF.fetch(`https://worker.test/v1/rooms/${room.room_id}/join`, {
      method: "POST",
      headers: bootstrapHeaders,
      body: "{}"
    });
    expect(secondJoin.status).toBe(409);
  });

  it("creates a direct P2P room with TURN configured as fallback rather than forced relay", async () => {
    const room = await createDirectRoom();
    expect(room).toMatchObject({
      protocol: "cloudflare-direct.v1",
      role: "host",
      relay_only: false,
      turn_fallback: true
    });
    expect(String(room.participant_token)).toHaveLength(43);

    const response = await SELF.fetch(`https://worker.test/v1/p2p/rooms/${room.room_id}/join`, {
      method: "POST",
      headers: bootstrapHeaders,
      body: "{}"
    });
    expect(response.status).toBe(201);
    const participant = await response.json() as Record<string, unknown>;
    expect(participant).toMatchObject({
      room_id: room.room_id,
      host_id: room.host_id,
      role: "client",
      relay_only: false,
      turn_fallback: true
    });
  });

  it("accepts Unreal FGuid participant IDs without requiring RFC version bits", async () => {
    const unrealGuid = "12345678-abcd-0fed-0123-456789abcdef";
    const response = await SELF.fetch("https://worker.test/v1/p2p/rooms", {
      method: "POST",
      headers: { ...bootstrapHeaders, "X-WebRTC4Unreal-Participant-Id": unrealGuid },
      body: JSON.stringify({ room_name: "FGuid compatibility", max_participants: 2 })
    });
    expect(response.status).toBe(201);
    expect(await response.json()).toMatchObject({ participant_id: unrealGuid, host_id: unrealGuid });
  });

  it("prepares one UUIDv7 room for Flow primary and Cloudflare Direct standby", async () => {
    const roomId = "019abcde-1234-7abc-8def-0123456789ab";
    const hostId = "4fb4168a-6bc6-4d17-bca7-2c75fced3a18";
    const clientId = "f8ed56f4-b026-4cf8-a64d-6ca6b068c2d8";
    let credentialSequence = 0;
    let createdFlowRoomName = "";

    vi.spyOn(globalThis, "fetch").mockImplementation(async (input, init) => {
      const url = input instanceof Request ? input.url : String(input);
      const method = init?.method ?? (input instanceof Request ? input.method : "GET");
      if (url === "https://management.example.test/access-credentials" && method === "POST") {
        credentialSequence += 1;
        return new Response(JSON.stringify({
          context_id: `test-context-${credentialSequence}`,
          headers: {
            "x-flow-principal": `test-principal-${credentialSequence}`,
            "x-flow-timestamp": `test-timestamp-${credentialSequence}`,
            "x-flow-signature": `test-signature-${credentialSequence}`
          }
        }), { status: 200, headers: { "Content-Type": "application/json" } });
      }
      if (url === "https://flow.example.test/v1/rooms" && method === "POST") {
        const body = JSON.parse(String(init?.body)) as Record<string, unknown>;
        createdFlowRoomName = String(body.name);
        return new Response(JSON.stringify({
          id: roomId,
          name: "Hybrid test",
          max_participants: 4
        }), { status: 201, headers: { "Content-Type": "application/json" } });
      }
      if (url === `https://flow.example.test/v1/rooms/${roomId}/join` && method === "POST") {
        return new Response(JSON.stringify({
          room_id: roomId,
          mode: "p2p",
          connection: {
            type: "p2p",
            protocol: "flow-signaling.v1",
            urls: [`wss://flow.example.test/v1/signal/${roomId}`],
            asyncapi_urls: ["https://flow.example.test/asyncapi.json"],
            ice: {
              expires_at: "2099-01-01T00:00:00Z",
              ice_servers: [
                { urls: ["stun:flow.example.test:3478"] },
                {
                  urls: [
                    "turn:flow.example.test:3478?transport=udp",
                    "turn:flow.example.test:3478?transport=tcp"
                  ],
                  username: `turn-user-${credentialSequence}`,
                  credential: `turn-password-${credentialSequence}`
                }
              ]
            }
          }
        }), { status: 200, headers: { "Content-Type": "application/json" } });
      }
      return new Response(JSON.stringify({ message: `Unexpected upstream ${method} ${url}` }), {
        status: 500,
        headers: { "Content-Type": "application/json" }
      });
    });

    const hostFlowResponse = await SELF.fetch(
      "https://worker.test/api/p2p/sessions/host?room_name=Hybrid%20test&max_participants=4",
      { headers: { ...bootstrapHeaders, "X-WebRTC4Unreal-Participant-Id": hostId } }
    );
    expect(hostFlowResponse.status).toBe(200);
    const hostFlow = await hostFlowResponse.json() as Record<string, unknown>;
    expect(hostFlow).toMatchObject({
      room_id: roomId,
      local_principal_id: hostId,
      host_principal_id: hostId,
      protocol: "flow-signaling.v1",
      relay_only: false,
      turn_fallback: true
    });
    expect(createdFlowRoomName).toBe("Hybridtest");

    const standbyResponse = await SELF.fetch("https://worker.test/v1/p2p/rooms", {
      method: "POST",
      headers: { ...bootstrapHeaders, "X-WebRTC4Unreal-Participant-Id": hostId },
      body: JSON.stringify({ room_id: roomId, room_name: "Hybrid test", max_participants: 4 })
    });
    expect(standbyResponse.status).toBe(201);
    const standby = await standbyResponse.json() as Record<string, unknown>;
    expect(standby).toMatchObject({ room_id: roomId, participant_id: hostId, host_id: hostId });

    const joinFlowResponse = await SELF.fetch(
      `https://worker.test/api/p2p/sessions/join?room_id=${roomId}`,
      { headers: { ...bootstrapHeaders, "X-WebRTC4Unreal-Participant-Id": clientId } }
    );
    expect(joinFlowResponse.status).toBe(200);
    const joinFlow = await joinFlowResponse.json() as Record<string, unknown>;
    expect(joinFlow).toMatchObject({
      room_id: roomId,
      local_principal_id: clientId,
      host_principal_id: hostId,
      protocol: "flow-signaling.v1"
    });

    const joinStandbyResponse = await SELF.fetch(
      `https://worker.test/v1/p2p/rooms/${roomId}/join`,
      {
        method: "POST",
        headers: { ...bootstrapHeaders, "X-WebRTC4Unreal-Participant-Id": clientId },
        body: "{}"
      }
    );
    expect(joinStandbyResponse.status).toBe(201);
    expect(await joinStandbyResponse.json()).toMatchObject({
      room_id: roomId,
      participant_id: clientId,
      host_id: hostId
    });
    expect(credentialSequence).toBe(2);
  });

  it("forwards SDP without stripping its final CRLF", async () => {
    const room = await createDirectRoom();
    const joinResponse = await SELF.fetch(`https://worker.test/v1/p2p/rooms/${room.room_id}/join`, {
      method: "POST",
      headers: bootstrapHeaders,
      body: "{}"
    });
    const participant = await joinResponse.json() as Record<string, unknown>;

    const hostSocket = await connectDirectParticipant(room);
    await receiveFrame(hostSocket);
    const hostNoticePromise = receiveFrame(hostSocket);
    const clientSocket = await connectDirectParticipant(participant);
    await receiveFrame(clientSocket);
    await hostNoticePromise;

    const sdp = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n";
    const forwardedPromise = receiveFrame(clientSocket);
    hostSocket.send(JSON.stringify({
      type: "p2p_offer",
      target_peer_id: participant.participant_id,
      sdp
    }));
    const forwarded = await forwardedPromise;
    expect(forwarded.sdp).toBe(sdp);

    clientSocket.close(1000, "test complete");
    hostSocket.close(1000, "test complete");
  });
});
