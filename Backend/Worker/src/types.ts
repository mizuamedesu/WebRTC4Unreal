/** SFU credentials are optional; Direct P2P only needs the required generated bindings. */
export type WorkerEnv = CloudflareBindings & {
  CALLS_APP_ID?: string;
  CALLS_APP_SECRET?: string;
};

export type JsonObject = Record<string, unknown>;

export interface ParticipantView {
  participant_id: string;
  is_host: boolean;
  session_id: string;
  realtime_ready: boolean;
  host_id: string;
  host_session_id: string;
  room_name: string;
  max_participants: number;
  participants: Array<{
    participant_id: string;
    is_host: boolean;
    session_id: string;
    realtime_ready: boolean;
  }>;
}

export interface RealtimeResult {
  status: number;
  body: JsonObject;
}

export interface DirectParticipantView {
  participant_id: string;
  is_host: boolean;
  host_id: string;
  room_name: string;
  max_participants: number;
  participants: Array<{
    participant_id: string;
    is_host: boolean;
    connected: boolean;
  }>;
}
