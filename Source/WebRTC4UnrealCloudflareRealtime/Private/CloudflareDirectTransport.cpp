#include "CloudflareDirectTransport.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "PixelStreamingDataChannel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WebSocketsModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogCloudflareDirect, Log, All);

namespace
{
	constexpr uint8 DirectUnrealPacketMessageType = 246;
	constexpr uint8 DirectChannelProbeMessageType = 0;
	constexpr int32 DirectMaxQueuedCandidates = 256;

	template <typename CallableType>
	void DispatchToGameThread(TWeakPtr<FCloudflareDirectTransport> WeakTransport,
		CallableType&& Callable)
	{
		TFunction<void(FCloudflareDirectTransport&)> Operation(Forward<CallableType>(Callable));
		if (IsInGameThread())
		{
			if (const TSharedPtr<FCloudflareDirectTransport> Transport = WeakTransport.Pin())
			{
				Operation(*Transport);
			}
			return;
		}
		AsyncTask(ENamedThreads::GameThread,
			[WeakTransport, Operation = MoveTemp(Operation)]() mutable
			{
				if (const TSharedPtr<FCloudflareDirectTransport> Transport = WeakTransport.Pin())
				{
					Operation(*Transport);
				}
			});
	}

	FString SerializeDirectSdp(const webrtc::SessionDescriptionInterface* Description)
	{
		std::string Serialized;
		if (!Description || !Description->ToString(&Serialized)) return FString();
		// Direct peers run the same libwebrtc build. Preserve its native SDP
		// serialization exactly; line-ending rewriting can make its parser reject
		// otherwise valid application/DataChannel offers.
		return UTF8_TO_TCHAR(Serialized.c_str());
	}

	FString ReadDirectTransportString(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		FString Value;
		if (Object) Object->TryGetStringField(FieldName, Value);
		return Value;
	}

	bool IsDirectPort53Url(const FString& Url)
	{
		return Url.EndsWith(TEXT(":53"), ESearchCase::IgnoreCase)
			|| Url.Contains(TEXT(":53?"), ESearchCase::IgnoreCase);
	}
}

FCloudflareDirectTransport::FCloudflareDirectTransport() = default;

FCloudflareDirectTransport::~FCloudflareDirectTransport()
{
	Close();
}

bool FCloudflareDirectTransport::Start(
	const TSharedRef<IWebRTC4UnrealTransportContext>& InContext, bool bHost,
	FWebRTC4UnrealTransportCallbacks InCallbacks, FString& Error)
{
	Context = StaticCastSharedRef<FCloudflareDirectTransportContext>(InContext);
	if (!Context || Context->Protocol != TEXT("cloudflare-direct.v1")
		|| Context->WorkerUrl.IsEmpty() || Context->SignalUrl.IsEmpty()
		|| Context->RoomId.IsEmpty() || Context->ParticipantId.IsEmpty()
		|| Context->ParticipantToken.IsEmpty() || Context->HostId.IsEmpty())
	{
		Error = TEXT("Cloudflare Direct transport context is incomplete");
		Context.Reset();
		return false;
	}
	if (bHost != (Context->ParticipantId == Context->HostId))
	{
		Error = TEXT("Cloudflare Direct transport role does not match the participant identity");
		Context.Reset();
		return false;
	}

	bIsHost = bHost;
	bClosing = false;
	bClosedNotified = false;
	Callbacks = MoveTemp(InCallbacks);
	RequestIceServers();
	return true;
}

bool FCloudflareDirectTransport::Send(const FString& PeerId,
	const uint8* Data, int32 NumBytes)
{
	const TSharedPtr<FPeerState>* Found = Peers.Find(PeerId);
	const TSharedPtr<FPeerState> Peer = Found ? *Found : nullptr;
	if (!Peer || !Peer->bConnectedNotified || !Peer->bChannelOpen
		|| !Peer->DataChannel || !Data || NumBytes <= 0)
	{
		return false;
	}
	TArray64<uint8> Packet;
	Packet.Append(Data, NumBytes);
	return Peer->DataChannel->SendArbitraryData(DirectUnrealPacketMessageType, Packet);
}

void FCloudflareDirectTransport::ClosePeer(const FString& PeerId)
{
	if (bIsHost && ControlSocket && ControlSocket->IsConnected())
	{
		const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("type"), TEXT("close_peer"));
		Frame->SetStringField(TEXT("target_peer_id"), PeerId);
		SendControl(Frame);
	}
	RemovePeer(PeerId, true);
}

void FCloudflareDirectTransport::Close()
{
	if (bClosing) return;
	if (Context)
	{
		SendWorkerRequest(TEXT(""), TEXT("DELETE"), nullptr,
			[](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>, bool) {});
	}
	bClosing = true;
	Peers.Reset();
	OrphanRemoteCandidates.Reset();

	if (ControlSocket)
	{
		ControlSocket->OnConnected().Remove(ControlConnectedHandle);
		ControlSocket->OnConnectionError().Remove(ControlErrorHandle);
		ControlSocket->OnClosed().Remove(ControlClosedHandle);
		ControlSocket->OnMessage().Remove(ControlMessageHandle);
		ControlSocket->Close(1000, TEXT("WebRTC4Unreal Cloudflare Direct transport closed"));
		ControlSocket.Reset();
	}
	NotifyClosed();
}

bool FCloudflareDirectTransport::IsValid() const
{
	return !bClosing && Context.IsValid() && bIceRequestStarted;
}

void FCloudflareDirectTransport::RequestIceServers()
{
	if (bIceRequestStarted) return;
	bIceRequestStarted = true;
	TWeakPtr<FCloudflareDirectTransport> WeakThis = AsShared();
	SendWorkerRequest(TEXT("/ice-servers"), TEXT("POST"), MakeShared<FJsonObject>(),
		[WeakThis](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded)
		{
			DispatchToGameThread(WeakThis,
				[Response, bSucceeded](FCloudflareDirectTransport& Self)
			{
				if (Self.bClosing) return;
				FString Error;
				if (!Self.ParseIceConfiguration(Response, bSucceeded, Error))
				{
					Self.ReportEndpointError(Error);
					return;
				}
				Self.ConnectControlWebSocket();
			});
		});
}

bool FCloudflareDirectTransport::ParseIceConfiguration(
	const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
	bool bTransportSucceeded, FString& Error)
{
	if (!bTransportSucceeded || !Response || Response->GetResponseCode() != 200)
	{
		Error = Response
			? FString::Printf(TEXT("TURN credential request failed with HTTP %d"), Response->GetResponseCode())
			: TEXT("TURN credential request could not reach the Worker");
		return false;
	}
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json)
	{
		Error = TEXT("Worker returned invalid ICE configuration JSON");
		return false;
	}
	FString Policy;
	if (!Json->TryGetStringField(TEXT("iceTransportPolicy"), Policy)
		|| Policy != TEXT("all"))
	{
		Error = TEXT("Worker attempted to force an unsupported ICE transport policy");
		return false;
	}
	bool bRelayOnly = true;
	if (!Json->TryGetBoolField(TEXT("relayOnly"), bRelayOnly) || bRelayOnly)
	{
		Error = TEXT("Worker attempted to force TURN relay-only mode");
		return false;
	}

	IceConfig = webrtc::PeerConnectionInterface::RTCConfiguration();
	IceConfig.type = webrtc::PeerConnectionInterface::kAll;
	IceConfig.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
	IceConfig.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
	const TArray<TSharedPtr<FJsonValue>>* Servers = nullptr;
	if (!Json->TryGetArrayField(TEXT("iceServers"), Servers) || !Servers)
	{
		Error = TEXT("Worker did not return any ICE servers");
		return false;
	}
	int32 StunUrls = 0;
	int32 TurnUrls = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Servers)
	{
		const TSharedPtr<FJsonObject>* ServerJson = nullptr;
		if (!Value || !Value->TryGetObject(ServerJson) || !ServerJson || !*ServerJson) continue;
		TArray<FString> Urls;
		if (!(*ServerJson)->TryGetStringArrayField(TEXT("urls"), Urls))
		{
			FString SingleUrl;
			if ((*ServerJson)->TryGetStringField(TEXT("urls"), SingleUrl)) Urls.Add(SingleUrl);
		}
		webrtc::PeerConnectionInterface::IceServer Server;
		for (const FString& Url : Urls)
		{
			if (IsDirectPort53Url(Url)) continue;
			if (Url.StartsWith(TEXT("stun:"), ESearchCase::IgnoreCase)) ++StunUrls;
			else if (Url.StartsWith(TEXT("turn:"), ESearchCase::IgnoreCase)
				|| Url.StartsWith(TEXT("turns:"), ESearchCase::IgnoreCase)) ++TurnUrls;
			else continue;
			Server.urls.push_back(TCHAR_TO_UTF8(*Url));
		}
		if (Server.urls.empty()) continue;
		FString Username;
		FString Credential;
		(*ServerJson)->TryGetStringField(TEXT("username"), Username);
		(*ServerJson)->TryGetStringField(TEXT("credential"), Credential);
		if (!Username.IsEmpty() && !Credential.IsEmpty())
		{
			Server.username = TCHAR_TO_UTF8(*Username);
			Server.password = TCHAR_TO_UTF8(*Credential);
		}
		IceConfig.servers.push_back(MoveTemp(Server));
	}
	if (StunUrls <= 0 || TurnUrls <= 0)
	{
		Error = TEXT("ICE configuration must contain both STUN and TURN fallback URLs");
		return false;
	}
	UE_LOG(LogCloudflareDirect, Display,
		TEXT("WEBRTC4UNREAL_DIRECT_ICE_CONFIGURED room=%s policy=all forced_turn=0 stun_urls=%d turn_urls=%d"),
		*Context->RoomId, StunUrls, TurnUrls);
	return true;
}

void FCloudflareDirectTransport::ConnectControlWebSocket()
{
	FWebSocketsModule& WebSockets =
		FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Authorization"), TEXT("Bearer ") + Context->ParticipantToken);
	Headers.Add(TEXT("X-WebRTC4Unreal-Participant"), Context->ParticipantId);
	ControlSocket = WebSockets.CreateWebSocket(Context->SignalUrl, TEXT(""), Headers);
	if (!ControlSocket)
	{
		ReportEndpointError(TEXT("Could not create the Cloudflare Direct control WebSocket"));
		return;
	}

	TWeakPtr<FCloudflareDirectTransport> WeakThis = AsShared();
	ControlConnectedHandle = ControlSocket->OnConnected().AddLambda([WeakThis]()
	{
		DispatchToGameThread(WeakThis, [](FCloudflareDirectTransport& Self)
		{
			if (!Self.bClosing) Self.OnControlConnected();
		});
	});
	ControlErrorHandle = ControlSocket->OnConnectionError().AddLambda(
		[WeakThis](const FString& InError)
		{
			DispatchToGameThread(WeakThis, [InError](FCloudflareDirectTransport& Self)
			{
				if (!Self.bClosing) Self.OnControlConnectionError(InError);
			});
		});
	ControlClosedHandle = ControlSocket->OnClosed().AddLambda(
		[WeakThis](int32 StatusCode, const FString& Reason, bool bWasClean)
		{
			DispatchToGameThread(WeakThis,
				[StatusCode, Reason, bWasClean](FCloudflareDirectTransport& Self)
			{
				if (!Self.bClosing) Self.OnControlClosed(StatusCode, Reason, bWasClean);
			});
		});
	ControlMessageHandle = ControlSocket->OnMessage().AddLambda(
		[WeakThis](const FString& Message)
		{
			DispatchToGameThread(WeakThis, [Message](FCloudflareDirectTransport& Self)
			{
				if (!Self.bClosing) Self.OnControlMessage(Message);
			});
		});
	ControlSocket->Connect();
}

void FCloudflareDirectTransport::OnControlConnected()
{
	UE_LOG(LogCloudflareDirect, Display,
		TEXT("WEBRTC4UNREAL_DIRECT_CONTROL_CONNECTED role=%s room=%s route=signaling-only"),
		bIsHost ? TEXT("host") : TEXT("client"), *Context->RoomId);
}

void FCloudflareDirectTransport::OnControlConnectionError(const FString& Error)
{
	ReportEndpointError(FString::Printf(TEXT("Cloudflare Direct control WebSocket failed: %s"), *Error));
}

void FCloudflareDirectTransport::OnControlClosed(int32 StatusCode,
	const FString& Reason, bool bWasClean)
{
	ReportEndpointError(FString::Printf(
		TEXT("Cloudflare Direct control WebSocket closed (status=%d clean=%d): %s"),
		StatusCode, bWasClean, *Reason));
}

void FCloudflareDirectTransport::OnControlMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Frame;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Frame) || !Frame)
	{
		ReportEndpointError(TEXT("Cloudflare Direct control WebSocket returned malformed JSON"));
		return;
	}
	const FString Type = ReadDirectTransportString(Frame, TEXT("type"));
	if (Type == TEXT("connected"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Participants = nullptr;
		if (bIsHost && Frame->TryGetArrayField(TEXT("participants"), Participants) && Participants)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Participants)
			{
				const TSharedPtr<FJsonObject>* Participant = nullptr;
				if (!Value || !Value->TryGetObject(Participant) || !Participant || !*Participant) continue;
				bool bParticipantHost = false;
				bool bConnected = false;
				(*Participant)->TryGetBoolField(TEXT("is_host"), bParticipantHost);
				(*Participant)->TryGetBoolField(TEXT("connected"), bConnected);
				if (!bParticipantHost && bConnected)
				{
					BeginHostOffer(ReadDirectTransportString(*Participant, TEXT("participant_id")));
				}
			}
		}
		return;
	}
	if (Type == TEXT("participant_connected") && bIsHost)
	{
		BeginHostOffer(ReadDirectTransportString(Frame, TEXT("peer_id")));
		return;
	}
	if (Type == TEXT("p2p_offer") && !bIsHost)
	{
		const FString PeerId = ReadDirectTransportString(Frame, TEXT("peer_id"));
		if (PeerId != Context->HostId)
		{
			ReportEndpointError(TEXT("Direct P2P offer did not come from the room host"));
			return;
		}
		ReceiveOffer(PeerId, ReadDirectTransportString(Frame, TEXT("sdp")));
		return;
	}
	if (Type == TEXT("p2p_answer") && bIsHost)
	{
		ReceiveAnswer(ReadDirectTransportString(Frame, TEXT("peer_id")),
			ReadDirectTransportString(Frame, TEXT("sdp")));
		return;
	}
	if (Type == TEXT("p2p_ice"))
	{
		FIceCandidate Candidate;
		Candidate.Candidate = ReadDirectTransportString(Frame, TEXT("candidate"));
		Candidate.SdpMid = ReadDirectTransportString(Frame, TEXT("sdp_mid"));
		Frame->TryGetNumberField(TEXT("sdp_mline_index"), Candidate.SdpMLineIndex);
		OnRemoteIceCandidate(ReadDirectTransportString(Frame, TEXT("peer_id")), MoveTemp(Candidate));
		return;
	}
	if (Type == TEXT("peer_left") || Type == TEXT("peer_disconnected") || Type == TEXT("peer_closed"))
	{
		const FString PeerId = Type == TEXT("peer_closed")
			? Context->HostId : ReadDirectTransportString(Frame, TEXT("peer_id"));
		if (!PeerId.IsEmpty()) RemovePeer(PeerId, true);
		return;
	}
	if (Type == TEXT("error"))
	{
		ReportEndpointError(FString::Printf(TEXT("Cloudflare Direct room control error: %s"),
			*ReadDirectTransportString(Frame, TEXT("code"))));
	}
}

bool FCloudflareDirectTransport::SendControl(const TSharedRef<FJsonObject>& Frame)
{
	if (!ControlSocket || !ControlSocket->IsConnected()) return false;
	FString Message;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Message);
	if (!FJsonSerializer::Serialize(Frame, Writer)) return false;
	ControlSocket->Send(Message);
	return true;
}

TSharedPtr<FCloudflareDirectTransport::FPeerState>
FCloudflareDirectTransport::CreatePeer(const FString& PeerId, bool bCreateDataChannel)
{
	if (PeerId.IsEmpty() || PeerId == Context->ParticipantId) return nullptr;
	if (const TSharedPtr<FPeerState>* Existing = Peers.Find(PeerId)) return *Existing;

	const TSharedRef<FPeerState> Peer = MakeShared<FPeerState>();
	Peer->PeerConnection = FPixelStreamingPeerConnection::Create(IceConfig, false);
	if (!Peer->PeerConnection)
	{
		ReportPeerError(PeerId, TEXT("Could not create a direct WebRTC PeerConnection"));
		return nullptr;
	}
	Peers.Add(PeerId, Peer);
	if (TArray<FIceCandidate>* Orphans = OrphanRemoteCandidates.Find(PeerId))
	{
		Peer->PendingRemoteCandidates = MoveTemp(*Orphans);
		OrphanRemoteCandidates.Remove(PeerId);
	}

	TWeakPtr<FCloudflareDirectTransport> WeakThis = AsShared();
	Peer->PeerConnection->OnEmitIceCandidate.AddLambda(
		[WeakThis, PeerId](const webrtc::IceCandidateInterface* Candidate)
		{
			if (!Candidate) return;
			std::string Serialized;
			if (!Candidate->ToString(&Serialized)) return;
			FIceCandidate Value;
			Value.Candidate = UTF8_TO_TCHAR(Serialized.c_str());
			Value.SdpMid = UTF8_TO_TCHAR(Candidate->sdp_mid().c_str());
			Value.SdpMLineIndex = Candidate->sdp_mline_index();
			DispatchToGameThread(WeakThis,
				[PeerId, Value = MoveTemp(Value)](FCloudflareDirectTransport& Self) mutable
			{
				if (!Self.bClosing) Self.OnLocalIceCandidate(PeerId, MoveTemp(Value));
			});
		});
	Peer->PeerConnection->OnNewDataChannel.AddLambda(
		[WeakThis, PeerId](TSharedPtr<FPixelStreamingDataChannel> Channel)
		{
			DispatchToGameThread(WeakThis,
				[PeerId, Channel](FCloudflareDirectTransport& Self)
			{
				if (!Self.bClosing) Self.BindDataChannel(PeerId, Channel);
			});
		});
	Peer->PeerConnection->OnIceStateChanged.AddLambda(
		[WeakThis, PeerId](webrtc::PeerConnectionInterface::IceConnectionState State)
		{
			DispatchToGameThread(WeakThis,
				[PeerId, State](FCloudflareDirectTransport& Self)
			{
				if (!Self.bClosing) Self.OnIceStateChanged(PeerId, State);
			});
		});
	if (bCreateDataChannel)
	{
		BindDataChannel(PeerId, Peer->PeerConnection->CreateDataChannel(0, false));
	}
	return Peer;
}

void FCloudflareDirectTransport::BeginHostOffer(const FString& PeerId)
{
	if (!bIsHost || PeerId.IsEmpty()) return;
	const TSharedPtr<FPeerState> Peer = CreatePeer(PeerId, true);
	if (!Peer || Peer->bOfferStarted) return;
	Peer->bOfferStarted = true;
	TWeakPtr<FCloudflareDirectTransport> WeakThis = AsShared();
	Peer->PeerConnection->CreateOffer(FPixelStreamingPeerConnection::EReceiveMediaOption::Nothing,
		[WeakThis, PeerId](const webrtc::SessionDescriptionInterface* Description)
		{
			const FString Sdp = SerializeDirectSdp(Description);
			DispatchToGameThread(WeakThis, [PeerId, Sdp](FCloudflareDirectTransport& Self)
			{
				if (!Self.bClosing) Self.SendLocalDescription(PeerId, TEXT("p2p_offer"), Sdp);
			});
		},
		[WeakThis, PeerId](const FString& Error)
		{
			DispatchToGameThread(WeakThis, [PeerId, Error](FCloudflareDirectTransport& Self)
			{
				Self.ReportPeerError(PeerId, FString::Printf(TEXT("Could not create P2P offer: %s"), *Error));
			});
		});
}

void FCloudflareDirectTransport::ReceiveOffer(const FString& PeerId, const FString& Sdp)
{
	if (bIsHost || Sdp.IsEmpty()) return;
	const TSharedPtr<FPeerState> Peer = CreatePeer(PeerId, false);
	if (!Peer || Peer->bAnswerStarted) return;
	TWeakPtr<FCloudflareDirectTransport> WeakThis = AsShared();
	Peer->PeerConnection->ReceiveOffer(Sdp,
		[WeakThis, PeerId]()
		{
			DispatchToGameThread(WeakThis, [PeerId](FCloudflareDirectTransport& Self)
			{
				const TSharedPtr<FPeerState>* Found = Self.Peers.Find(PeerId);
				const TSharedPtr<FPeerState> Current = Found ? *Found : nullptr;
				if (!Current || Self.bClosing) return;
				Current->bRemoteDescriptionSet = true;
				Self.FlushRemoteIceCandidates(PeerId, Current.ToSharedRef());
				Current->bAnswerStarted = true;
				TWeakPtr<FCloudflareDirectTransport> InnerWeak = Self.AsShared();
				Current->PeerConnection->CreateAnswer(
					FPixelStreamingPeerConnection::EReceiveMediaOption::Nothing,
					[InnerWeak, PeerId](const webrtc::SessionDescriptionInterface* Description)
					{
						const FString AnswerSdp = SerializeDirectSdp(Description);
						DispatchToGameThread(InnerWeak,
							[PeerId, AnswerSdp](FCloudflareDirectTransport& Inner)
						{
							if (!Inner.bClosing)
								Inner.SendLocalDescription(PeerId, TEXT("p2p_answer"), AnswerSdp);
						});
					},
					[InnerWeak, PeerId](const FString& Error)
					{
						DispatchToGameThread(InnerWeak, [PeerId, Error](FCloudflareDirectTransport& Inner)
						{
							Inner.ReportPeerError(PeerId,
								FString::Printf(TEXT("Could not create P2P answer: %s"), *Error));
						});
					});
			});
		},
		[WeakThis, PeerId](const FString& Error)
		{
			DispatchToGameThread(WeakThis, [PeerId, Error](FCloudflareDirectTransport& Self)
			{
				Self.ReportPeerError(PeerId, FString::Printf(TEXT("Could not receive P2P offer: %s"), *Error));
			});
		});
}

void FCloudflareDirectTransport::ReceiveAnswer(const FString& PeerId, const FString& Sdp)
{
	const TSharedPtr<FPeerState>* Found = Peers.Find(PeerId);
	const TSharedPtr<FPeerState> Peer = Found ? *Found : nullptr;
	if (!bIsHost || !Peer || Sdp.IsEmpty()) return;
	TWeakPtr<FCloudflareDirectTransport> WeakThis = AsShared();
	Peer->PeerConnection->ReceiveAnswer(Sdp,
		[WeakThis, PeerId]()
		{
			DispatchToGameThread(WeakThis, [PeerId](FCloudflareDirectTransport& Self)
			{
				const TSharedPtr<FPeerState>* CurrentValue = Self.Peers.Find(PeerId);
				const TSharedPtr<FPeerState> Current = CurrentValue ? *CurrentValue : nullptr;
				if (!Current || Self.bClosing) return;
				Current->bRemoteDescriptionSet = true;
				Self.FlushRemoteIceCandidates(PeerId, Current.ToSharedRef());
			});
		},
		[WeakThis, PeerId](const FString& Error)
		{
			DispatchToGameThread(WeakThis, [PeerId, Error](FCloudflareDirectTransport& Self)
			{
				Self.ReportPeerError(PeerId, FString::Printf(TEXT("Could not receive P2P answer: %s"), *Error));
			});
		});
}

void FCloudflareDirectTransport::SendLocalDescription(const FString& PeerId,
	const FString& Type, const FString& Sdp)
{
	const TSharedPtr<FPeerState>* Found = Peers.Find(PeerId);
	const TSharedPtr<FPeerState> Peer = Found ? *Found : nullptr;
	if (!Peer || Sdp.IsEmpty())
	{
		ReportPeerError(PeerId, TEXT("WebRTC returned an empty local session description"));
		return;
	}
	const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(TEXT("type"), Type);
	Frame->SetStringField(TEXT("target_peer_id"), PeerId);
	Frame->SetStringField(TEXT("sdp"), Sdp);
	if (!SendControl(Frame))
	{
		ReportPeerError(PeerId, TEXT("Could not send the direct P2P session description"));
		return;
	}
	Peer->bLocalDescriptionSent = true;
	FlushLocalIceCandidates(PeerId, Peer.ToSharedRef());
}

void FCloudflareDirectTransport::OnLocalIceCandidate(const FString& PeerId,
	FIceCandidate Candidate)
{
	const TSharedPtr<FPeerState>* Found = Peers.Find(PeerId);
	const TSharedPtr<FPeerState> Peer = Found ? *Found : nullptr;
	if (!Peer || Candidate.Candidate.IsEmpty()) return;
	if (!Peer->bLocalDescriptionSent)
	{
		if (Peer->PendingLocalCandidates.Num() < DirectMaxQueuedCandidates)
			Peer->PendingLocalCandidates.Add(MoveTemp(Candidate));
		return;
	}
	const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(TEXT("type"), TEXT("p2p_ice"));
	Frame->SetStringField(TEXT("target_peer_id"), PeerId);
	Frame->SetStringField(TEXT("candidate"), Candidate.Candidate);
	Frame->SetStringField(TEXT("sdp_mid"), Candidate.SdpMid);
	Frame->SetNumberField(TEXT("sdp_mline_index"), Candidate.SdpMLineIndex);
	if (!SendControl(Frame)) ReportPeerError(PeerId, TEXT("Could not send a direct P2P ICE candidate"));
}

void FCloudflareDirectTransport::OnRemoteIceCandidate(const FString& PeerId,
	FIceCandidate Candidate)
{
	if (PeerId.IsEmpty() || Candidate.Candidate.IsEmpty()) return;
	const TSharedPtr<FPeerState>* Found = Peers.Find(PeerId);
	const TSharedPtr<FPeerState> Peer = Found ? *Found : nullptr;
	if (!Peer)
	{
		TArray<FIceCandidate>& Orphans = OrphanRemoteCandidates.FindOrAdd(PeerId);
		if (Orphans.Num() < DirectMaxQueuedCandidates) Orphans.Add(MoveTemp(Candidate));
		return;
	}
	if (!Peer->bRemoteDescriptionSet)
	{
		if (Peer->PendingRemoteCandidates.Num() < DirectMaxQueuedCandidates)
			Peer->PendingRemoteCandidates.Add(MoveTemp(Candidate));
		return;
	}
	Peer->PeerConnection->AddRemoteIceCandidate(
		Candidate.SdpMid, Candidate.SdpMLineIndex, Candidate.Candidate);
}

void FCloudflareDirectTransport::FlushLocalIceCandidates(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer)
{
	TArray<FIceCandidate> Candidates = MoveTemp(Peer->PendingLocalCandidates);
	Peer->PendingLocalCandidates.Reset();
	for (FIceCandidate& Candidate : Candidates) OnLocalIceCandidate(PeerId, MoveTemp(Candidate));
}

void FCloudflareDirectTransport::FlushRemoteIceCandidates(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer)
{
	TArray<FIceCandidate> Candidates = MoveTemp(Peer->PendingRemoteCandidates);
	Peer->PendingRemoteCandidates.Reset();
	for (FIceCandidate& Candidate : Candidates)
	{
		Peer->PeerConnection->AddRemoteIceCandidate(
			Candidate.SdpMid, Candidate.SdpMLineIndex, Candidate.Candidate);
	}
}

void FCloudflareDirectTransport::OnIceStateChanged(const FString& PeerId,
	webrtc::PeerConnectionInterface::IceConnectionState NewState)
{
	const TSharedPtr<FPeerState>* Found = Peers.Find(PeerId);
	const TSharedPtr<FPeerState> Peer = Found ? *Found : nullptr;
	if (!Peer) return;
	if (NewState == webrtc::PeerConnectionInterface::kIceConnectionConnected
		|| NewState == webrtc::PeerConnectionInterface::kIceConnectionCompleted)
	{
		Peer->bIceConnected = true;
		UE_LOG(LogCloudflareDirect, Display,
			TEXT("WEBRTC4UNREAL_DIRECT_ICE_CONNECTED role=%s peer=%s room=%s policy=all forced_turn=0"),
			bIsHost ? TEXT("host") : TEXT("client"), *PeerId, *Context->RoomId);
		MaybeNotifyPeerConnected(PeerId, Peer.ToSharedRef());
		return;
	}
	if (NewState == webrtc::PeerConnectionInterface::kIceConnectionDisconnected)
	{
		UE_LOG(LogCloudflareDirect, Warning,
			TEXT("WEBRTC4UNREAL_DIRECT_ICE_DISCONNECTED peer=%s room=%s"), *PeerId, *Context->RoomId);
		return;
	}
	if (NewState == webrtc::PeerConnectionInterface::kIceConnectionFailed
		|| NewState == webrtc::PeerConnectionInterface::kIceConnectionClosed)
	{
		ReportPeerError(PeerId, TEXT("Direct P2P ICE connection failed or closed"));
		RemovePeer(PeerId, true);
	}
}

void FCloudflareDirectTransport::BindDataChannel(const FString& PeerId,
	const TSharedPtr<FPixelStreamingDataChannel>& Channel)
{
	const TSharedPtr<FPeerState>* Found = Peers.Find(PeerId);
	const TSharedPtr<FPeerState> Peer = Found ? *Found : nullptr;
	if (!Peer || !Channel)
	{
		ReportPeerError(PeerId, TEXT("WebRTC could not create the direct DataChannel"));
		return;
	}
	if (Peer->DataChannel == Channel) return;
	Peer->DataChannel = Channel;
	TWeakPtr<FCloudflareDirectTransport> WeakThis = AsShared();
	Channel->OnOpen.AddLambda([WeakThis, PeerId](FPixelStreamingDataChannel&)
	{
		DispatchToGameThread(WeakThis, [PeerId](FCloudflareDirectTransport& Self)
		{
			if (!Self.bClosing) Self.OnDataChannelOpen(PeerId);
		});
	});
	Channel->OnClosed.AddLambda([WeakThis, PeerId](FPixelStreamingDataChannel&)
	{
		DispatchToGameThread(WeakThis, [PeerId](FCloudflareDirectTransport& Self)
		{
			if (!Self.bClosing) Self.RemovePeer(PeerId, true);
		});
	});
	Channel->OnMessageReceived.AddLambda(
		[WeakThis, PeerId](uint8 Type, const webrtc::DataBuffer& Buffer)
		{
			if (const TSharedPtr<FCloudflareDirectTransport> Self = WeakThis.Pin())
			{
				Self->HandleChannelMessage(PeerId, Type, Buffer);
			}
		});
	if (Channel->SendMessage(DirectChannelProbeMessageType)) OnDataChannelOpen(PeerId);
}

void FCloudflareDirectTransport::OnDataChannelOpen(const FString& PeerId)
{
	const TSharedPtr<FPeerState>* Found = Peers.Find(PeerId);
	const TSharedPtr<FPeerState> Peer = Found ? *Found : nullptr;
	if (!Peer || Peer->bChannelOpen) return;
	Peer->bChannelOpen = true;
	UE_LOG(LogCloudflareDirect, Display,
		TEXT("WEBRTC4UNREAL_DIRECT_DATACHANNEL_OPEN role=%s peer=%s room=%s route=p2p streams=1"),
		bIsHost ? TEXT("host") : TEXT("client"), *PeerId, *Context->RoomId);
	MaybeNotifyPeerConnected(PeerId, Peer.ToSharedRef());
}

void FCloudflareDirectTransport::HandleChannelMessage(const FString& PeerId,
	uint8 Type, const webrtc::DataBuffer& Buffer)
{
	constexpr int32 HeaderSize = sizeof(uint8) + sizeof(int32);
	if (Type != DirectUnrealPacketMessageType || Buffer.data.size() < HeaderSize) return;
	const uint8* Bytes = Buffer.data.data();
	int32 PayloadSize = 0;
	FMemory::Memcpy(&PayloadSize, Bytes + sizeof(uint8), sizeof(PayloadSize));
	if (PayloadSize <= 0 || PayloadSize > static_cast<int32>(Buffer.data.size()) - HeaderSize)
	{
		ReportPeerError(PeerId, TEXT("Received a malformed Unreal packet through direct WebRTC"));
		return;
	}
	TArray<uint8> Packet;
	Packet.Append(Bytes + HeaderSize, PayloadSize);
	if (Callbacks.OnPacket) Callbacks.OnPacket(PeerId, MoveTemp(Packet));
}

void FCloudflareDirectTransport::MaybeNotifyPeerConnected(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer)
{
	if (Peer->bConnectedNotified || !Peer->bIceConnected || !Peer->bChannelOpen) return;
	Peer->bConnectedNotified = true;
	UE_LOG(LogCloudflareDirect, Display,
		TEXT("WEBRTC4UNREAL_DIRECT_PEER_CONNECTED role=%s peer=%s room=%s route=p2p turn=fallback"),
		bIsHost ? TEXT("host") : TEXT("client"), *PeerId, *Context->RoomId);
	if (Callbacks.OnPeerConnected) Callbacks.OnPeerConnected(PeerId);
}

void FCloudflareDirectTransport::RemovePeer(const FString& PeerId, bool bNotify)
{
	TSharedPtr<FPeerState> Peer;
	if (!Peers.RemoveAndCopyValue(PeerId, Peer) || !Peer) return;
	Peer->bChannelOpen = false;
	Peer->DataChannel.Reset();
	Peer->PeerConnection.Reset();
	if (bNotify) NotifyPeerDisconnected(PeerId, Peer.ToSharedRef());
}

void FCloudflareDirectTransport::SendWorkerRequest(const FString& Suffix,
	const FString& Verb, const TSharedPtr<FJsonObject>& Body, FHttpCallback Callback)
{
	if (!Context)
	{
		Callback(nullptr, false);
		return;
	}
	const FString Path = FString::Printf(TEXT("/v1/p2p/rooms/%s/participants/%s%s"),
		*Context->RoomId, *Context->ParticipantId, *Suffix);
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	const TSharedRef<FHttpCallback, ESPMode::ThreadSafe> SharedCallback =
		MakeShared<FHttpCallback, ESPMode::ThreadSafe>(MoveTemp(Callback));
	Request->SetURL(Context->WorkerUrl + Path);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Context->ParticipantToken);
	Request->SetHeader(TEXT("Cache-Control"), TEXT("no-store"));
	if (Body)
	{
		FString SerializedBody;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&SerializedBody);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetContentAsString(SerializedBody);
	}
	Request->OnProcessRequestComplete().BindLambda(
		[SharedCallback](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			(*SharedCallback)(Response, bSucceeded);
		});
	if (!Request->ProcessRequest()) (*SharedCallback)(nullptr, false);
}

void FCloudflareDirectTransport::NotifyPeerDisconnected(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer)
{
	if (Peer->bDisconnectNotified) return;
	Peer->bDisconnectNotified = true;
	if (Callbacks.OnPeerDisconnected) Callbacks.OnPeerDisconnected(PeerId);
}

void FCloudflareDirectTransport::NotifyClosed()
{
	if (bClosedNotified) return;
	bClosedNotified = true;
	if (Callbacks.OnClosed) Callbacks.OnClosed();
}

void FCloudflareDirectTransport::ReportPeerError(const FString& PeerId,
	const FString& Error)
{
	UE_LOG(LogCloudflareDirect, Error,
		TEXT("WEBRTC4UNREAL_DIRECT_PEER_ERROR peer=%s room=%s message=%s"),
		*PeerId, Context ? *Context->RoomId : TEXT(""), *Error);
	if (Callbacks.OnError) Callbacks.OnError(PeerId, Error);
}

void FCloudflareDirectTransport::ReportEndpointError(const FString& Error)
{
	UE_LOG(LogCloudflareDirect, Error,
		TEXT("WEBRTC4UNREAL_DIRECT_TRANSPORT_ERROR room=%s message=%s"),
		Context ? *Context->RoomId : TEXT(""), *Error);
	if (Callbacks.OnError) Callbacks.OnError(TEXT(""), Error);
}
