#include "CloudflareRealtimeTransport.h"

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

DEFINE_LOG_CATEGORY_STATIC(LogCloudflareRealtime, Log, All);

namespace
{
	constexpr uint8 UnrealPacketMessageType = 246;
	constexpr uint8 CloudflareAckMessageType = 0;

	template <typename CallableType>
	void DispatchToGameThread(TWeakPtr<FCloudflareRealtimeTransport> WeakTransport,
		CallableType&& Callable)
	{
		TFunction<void(FCloudflareRealtimeTransport&)> Operation(Forward<CallableType>(Callable));
		if (IsInGameThread())
		{
			if (const TSharedPtr<FCloudflareRealtimeTransport> Transport = WeakTransport.Pin())
			{
				Operation(*Transport);
			}
			return;
		}
		AsyncTask(ENamedThreads::GameThread,
			[WeakTransport, Operation = MoveTemp(Operation)]() mutable
			{
				if (const TSharedPtr<FCloudflareRealtimeTransport> Transport = WeakTransport.Pin())
				{
					Operation(*Transport);
				}
			});
	}

	FString SerializeSdp(const webrtc::SessionDescriptionInterface* Description)
	{
		std::string Serialized;
		if (!Description || !Description->ToString(&Serialized)) return FString();
		FString Result = UTF8_TO_TCHAR(Serialized.c_str());
		Result.ReplaceInline(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
		Result.ReplaceInline(TEXT("\r"), TEXT("\n"), ESearchCase::CaseSensitive);
		Result.ReplaceInline(TEXT("\n"), TEXT("\r\n"), ESearchCase::CaseSensitive);
		if (!Result.EndsWith(TEXT("\r\n"), ESearchCase::CaseSensitive))
		{
			Result += TEXT("\r\n");
		}
		return Result;
	}
}

FCloudflareRealtimeTransport::FCloudflareRealtimeTransport() = default;

FCloudflareRealtimeTransport::~FCloudflareRealtimeTransport()
{
	Close();
}

bool FCloudflareRealtimeTransport::Start(
	const TSharedRef<IWebRTC4UnrealTransportContext>& InContext, bool bHost,
	FWebRTC4UnrealTransportCallbacks InCallbacks, FString& Error)
{
	Context = StaticCastSharedRef<FCloudflareRealtimeTransportContext>(InContext);
	if (!Context || Context->Protocol != TEXT("cloudflare-realtime.v1")
		|| Context->WorkerUrl.IsEmpty() || Context->SignalUrl.IsEmpty()
		|| Context->RoomId.IsEmpty() || Context->ParticipantId.IsEmpty()
		|| Context->ParticipantToken.IsEmpty() || Context->HostId.IsEmpty())
	{
		Error = TEXT("Cloudflare Realtime transport context is incomplete");
		Context.Reset();
		return false;
	}

	bIsHost = bHost;
	bClosing = false;
	bClosedNotified = false;
	Callbacks = MoveTemp(InCallbacks);
	ConfigurePeerConnection();
	if (!PeerConnection)
	{
		Error = TEXT("Could not create a WebRTC PeerConnection for Cloudflare Realtime");
		return false;
	}
	ConnectControlWebSocket();
	if (!ControlSocket)
	{
		Error = TEXT("Could not create the Cloudflare Realtime control WebSocket");
		PeerConnection.Reset();
		return false;
	}
	return true;
}

bool FCloudflareRealtimeTransport::Send(const FString& PeerId,
	const uint8* Data, int32 NumBytes)
{
	const TSharedPtr<FPeerState> Peer = FindPeer(PeerId);
	if (!Peer || !Peer->bConnectedNotified || !Peer->bSendDataChannelOpen
		|| !Peer->SendDataChannel || !Data || NumBytes <= 0)
	{
		return false;
	}
	TArray64<uint8> Packet;
	Packet.Append(Data, NumBytes);
	return Peer->SendDataChannel->SendArbitraryData(UnrealPacketMessageType, Packet);
}

void FCloudflareRealtimeTransport::ClosePeer(const FString& PeerId)
{
	const TSharedPtr<FPeerState> Peer = FindPeer(PeerId);
	if (!Peer) return;
	if (bIsHost && ControlSocket && ControlSocket->IsConnected())
	{
		const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("type"), TEXT("close_peer"));
		Frame->SetStringField(TEXT("target_peer_id"), PeerId);
		SendControl(Frame);
	}
	Peers.Remove(PeerId);
	Peer->bSendDataChannelOpen = false;
	Peer->bReceiveDataChannelOpen = false;
	Peer->SendDataChannel.Reset();
	Peer->ReceiveDataChannel.Reset();
	NotifyPeerDisconnected(PeerId, Peer.ToSharedRef());
}

void FCloudflareRealtimeTransport::Close()
{
	if (bClosing) return;
	if (Context && !SessionId.IsEmpty())
	{
		SendWorkerRequest(TEXT(""), TEXT("DELETE"), nullptr,
			[](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>, bool) {});
	}
	bClosing = true;

	for (const TPair<FString, TSharedPtr<FPeerState>>& Pair : Peers)
	{
		if (Pair.Value)
		{
			Pair.Value->bSendDataChannelOpen = false;
			Pair.Value->bReceiveDataChannelOpen = false;
			Pair.Value->SendDataChannel.Reset();
			Pair.Value->ReceiveDataChannel.Reset();
		}
	}
	Peers.Reset();
	ServerEventsChannel.Reset();
	PeerConnection.Reset();

	if (ControlSocket)
	{
		ControlSocket->OnConnected().Remove(ControlConnectedHandle);
		ControlSocket->OnConnectionError().Remove(ControlErrorHandle);
		ControlSocket->OnClosed().Remove(ControlClosedHandle);
		ControlSocket->OnMessage().Remove(ControlMessageHandle);
		ControlSocket->Close(1000, TEXT("WebRTC4Unreal Cloudflare Realtime transport closed"));
		ControlSocket.Reset();
	}
	NotifyClosed();
}

bool FCloudflareRealtimeTransport::IsValid() const
{
	return !bClosing && PeerConnection.IsValid() && ControlSocket.IsValid();
}

void FCloudflareRealtimeTransport::ConfigurePeerConnection()
{
	IceConfig = webrtc::PeerConnectionInterface::RTCConfiguration();
	IceConfig.type = webrtc::PeerConnectionInterface::kAll;
	IceConfig.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
	IceConfig.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
	IceConfig.servers.push_back(webrtc::PeerConnectionInterface::IceServer{});
	IceConfig.servers.back().urls.push_back("stun:stun.cloudflare.com:3478");
	PeerConnection = FPixelStreamingPeerConnection::Create(IceConfig, true);
	if (!PeerConnection) return;

	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	PeerConnection->OnNewDataChannel.AddLambda(
		[WeakThis](TSharedPtr<FPixelStreamingDataChannel> Channel)
		{
			DispatchToGameThread(WeakThis,
				[Channel](FCloudflareRealtimeTransport& Self)
			{
				if (!Self.bClosing) Self.ServerEventsChannel = Channel;
			});
		});
	PeerConnection->OnIceStateChanged.AddLambda(
		[WeakThis](webrtc::PeerConnectionInterface::IceConnectionState State)
		{
			DispatchToGameThread(WeakThis,
				[State](FCloudflareRealtimeTransport& Self)
			{
				if (!Self.bClosing) Self.OnIceStateChanged(State);
			});
		});
	UE_LOG(LogCloudflareRealtime, Display,
		TEXT("WEBRTC4UNREAL_CLOUDFLARE_ICE_CONFIGURED stun=stun.cloudflare.com:3478 route=sfu room=%s"),
		*Context->RoomId);
}

void FCloudflareRealtimeTransport::ConnectControlWebSocket()
{
	FWebSocketsModule& WebSockets =
		FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Authorization"), TEXT("Bearer ") + Context->ParticipantToken);
	Headers.Add(TEXT("X-WebRTC4Unreal-Participant"), Context->ParticipantId);
	ControlSocket = WebSockets.CreateWebSocket(Context->SignalUrl, TEXT(""), Headers);
	if (!ControlSocket) return;

	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	ControlConnectedHandle = ControlSocket->OnConnected().AddLambda([WeakThis]()
	{
		DispatchToGameThread(WeakThis, [](FCloudflareRealtimeTransport& Self)
		{
			if (!Self.bClosing) Self.OnControlConnected();
		});
	});
	ControlErrorHandle = ControlSocket->OnConnectionError().AddLambda(
		[WeakThis](const FString& Error)
		{
			DispatchToGameThread(WeakThis, [Error](FCloudflareRealtimeTransport& Self)
			{
				if (!Self.bClosing) Self.OnControlConnectionError(Error);
			});
		});
	ControlClosedHandle = ControlSocket->OnClosed().AddLambda(
		[WeakThis](int32 StatusCode, const FString& Reason, bool bWasClean)
		{
			DispatchToGameThread(WeakThis,
				[StatusCode, Reason, bWasClean](FCloudflareRealtimeTransport& Self)
			{
				if (!Self.bClosing) Self.OnControlClosed(StatusCode, Reason, bWasClean);
			});
		});
	ControlMessageHandle = ControlSocket->OnMessage().AddLambda(
		[WeakThis](const FString& Message)
		{
			DispatchToGameThread(WeakThis, [Message](FCloudflareRealtimeTransport& Self)
			{
				if (!Self.bClosing) Self.OnControlMessage(Message);
			});
		});
	UE_LOG(LogCloudflareRealtime, Log,
		TEXT("WEBRTC4UNREAL_CLOUDFLARE_CONTROL_CONNECTING role=%s room=%s"),
		bIsHost ? TEXT("host") : TEXT("client"), *Context->RoomId);
	ControlSocket->Connect();
}

void FCloudflareRealtimeTransport::OnControlConnected()
{
	UE_LOG(LogCloudflareRealtime, Display,
		TEXT("WEBRTC4UNREAL_CLOUDFLARE_CONTROL_CONNECTED role=%s room=%s"),
		bIsHost ? TEXT("host") : TEXT("client"), *Context->RoomId);
	CreateRealtimeSession();
}

void FCloudflareRealtimeTransport::OnControlConnectionError(const FString& Error)
{
	ReportEndpointError(FString::Printf(TEXT("Cloudflare control WebSocket failed: %s"), *Error));
}

void FCloudflareRealtimeTransport::OnControlClosed(int32 StatusCode,
	const FString& Reason, bool bWasClean)
{
	ReportEndpointError(FString::Printf(
		TEXT("Cloudflare control WebSocket closed (status=%d clean=%d): %s"),
		StatusCode, bWasClean, *Reason));
}

void FCloudflareRealtimeTransport::OnControlMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Frame;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Frame) || !Frame)
	{
		ReportEndpointError(TEXT("Cloudflare control WebSocket returned malformed JSON"));
		return;
	}
	const FString Type = GetString(Frame, TEXT("type"));
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
				bool bRealtimeReady = false;
				(*Participant)->TryGetBoolField(TEXT("is_host"), bParticipantHost);
				(*Participant)->TryGetBoolField(TEXT("realtime_ready"), bRealtimeReady);
				if (!bParticipantHost && bRealtimeReady)
				{
					QueueHostPeer(GetString(*Participant, TEXT("participant_id")),
						GetString(*Participant, TEXT("session_id")));
				}
			}
		}
		return;
	}
	if (Type == TEXT("peer_ready") && bIsHost)
	{
		QueueHostPeer(GetString(Frame, TEXT("peer_id")), GetString(Frame, TEXT("session_id")));
		return;
	}
	if (Type == TEXT("channel_offer") && !bIsHost)
	{
		if (GetString(Frame, TEXT("peer_id")) != Context->HostId)
		{
			ReportEndpointError(TEXT("Cloudflare channel offer did not come from the room host"));
			return;
		}
		QueueClientChannel(GetString(Frame, TEXT("publisher_session_id")),
			GetString(Frame, TEXT("data_channel_name")));
		return;
	}
	if (Type == TEXT("channel_answer") && bIsHost)
	{
		const FString PeerId = GetString(Frame, TEXT("peer_id"));
		SubscribeHostChannel(PeerId,
			GetString(Frame, TEXT("publisher_session_id")),
			GetString(Frame, TEXT("data_channel_name")));
		return;
	}
	if (Type == TEXT("channel_ready"))
	{
		const FString PeerId = GetString(Frame, TEXT("peer_id"));
		const bool bExpectedPeer = bIsHost
			? PeerId != Context->ParticipantId
			: PeerId == Context->HostId;
		if (bExpectedPeer)
		{
			if (const TSharedPtr<FPeerState> Peer = FindPeer(PeerId))
			{
				Peer->bRemoteReady = true;
				MaybeNotifyPeerConnected(PeerId, Peer.ToSharedRef());
			}
		}
		return;
	}
	if (Type == TEXT("peer_left") || Type == TEXT("peer_disconnected") || Type == TEXT("peer_closed"))
	{
		const FString PeerId = Type == TEXT("peer_closed")
			? Context->HostId : GetString(Frame, TEXT("peer_id"));
		if (!PeerId.IsEmpty()) ClosePeer(PeerId);
		return;
	}
	if (Type == TEXT("error"))
	{
		ReportEndpointError(FString::Printf(TEXT("Cloudflare room control error: %s"),
			*GetString(Frame, TEXT("code"))));
	}
}

bool FCloudflareRealtimeTransport::SendControl(const TSharedRef<FJsonObject>& Frame)
{
	if (!ControlSocket || !ControlSocket->IsConnected()) return false;
	FString Message;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Message);
	if (!FJsonSerializer::Serialize(Frame, Writer)) return false;
	ControlSocket->Send(Message);
	return true;
}

void FCloudflareRealtimeTransport::CreateRealtimeSession()
{
	if (bSessionRequestStarted) return;
	bSessionRequestStarted = true;
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	SendWorkerRequest(TEXT("/session"), TEXT("POST"), MakeShared<FJsonObject>(),
		[WeakThis](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded)
		{
			DispatchToGameThread(WeakThis,
				[Response, bSucceeded](FCloudflareRealtimeTransport& Self)
			{
				if (Self.bClosing) return;
				TSharedPtr<FJsonObject> Json;
				FString Error;
				if (!Self.ParseJsonResponse(Response, bSucceeded, { 200, 201 }, Json, Error))
				{
					Self.ReportEndpointError(Error);
					return;
				}
				Self.SessionId = GetString(Json, TEXT("sessionId"));
				if (Self.SessionId.IsEmpty())
				{
					Self.ReportEndpointError(TEXT("Cloudflare Worker did not return a Realtime session ID"));
					return;
				}
				Self.EstablishDataChannelTransport();
			});
		});
}

void FCloudflareRealtimeTransport::EstablishDataChannelTransport()
{
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	SendWorkerRequest(TEXT("/realtime/establish"), TEXT("POST"), MakeShared<FJsonObject>(),
		[WeakThis](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded)
		{
			DispatchToGameThread(WeakThis,
				[Response, bSucceeded](FCloudflareRealtimeTransport& Self)
			{
				if (Self.bClosing) return;
				TSharedPtr<FJsonObject> Json;
				FString Error;
				if (!Self.ParseJsonResponse(Response, bSucceeded, { 200 }, Json, Error))
				{
					Self.ReportEndpointError(Error);
					return;
				}
				bool bImmediateRenegotiation = false;
				Json->TryGetBoolField(TEXT("requiresImmediateRenegotiation"), bImmediateRenegotiation);
				const TSharedPtr<FJsonObject>* Description = nullptr;
				if (!bImmediateRenegotiation
					|| !Json->TryGetObjectField(TEXT("sessionDescription"), Description)
					|| !Description || !*Description
					|| GetString(*Description, TEXT("type")) != TEXT("offer"))
				{
					Self.ReportEndpointError(TEXT("Cloudflare Realtime did not return the expected SDP offer"));
					return;
				}
				Self.AcceptRealtimeOffer(GetString(*Description, TEXT("sdp")));
			});
		});
}

void FCloudflareRealtimeTransport::AcceptRealtimeOffer(const FString& Sdp)
{
	if (!PeerConnection || Sdp.IsEmpty())
	{
		ReportEndpointError(TEXT("Cloudflare Realtime returned an empty SDP offer"));
		return;
	}
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	PeerConnection->ReceiveOffer(Sdp,
		[WeakThis]()
		{
			DispatchToGameThread(WeakThis, [WeakThis](FCloudflareRealtimeTransport& Self)
			{
				if (Self.bClosing || !Self.PeerConnection) return;
				Self.PeerConnection->CreateAnswer(
					FPixelStreamingPeerConnection::EReceiveMediaOption::Nothing,
					[WeakThis](const webrtc::SessionDescriptionInterface* Answer)
					{
						DispatchToGameThread(WeakThis,
							[Answer](FCloudflareRealtimeTransport& Pinned)
						{
							if (!Pinned.bClosing) Pinned.SendRealtimeAnswer(Answer);
						});
					},
					[WeakThis](const FString& Error)
					{
						DispatchToGameThread(WeakThis,
							[Error](FCloudflareRealtimeTransport& Pinned)
						{
							Pinned.ReportEndpointError(FString::Printf(
								TEXT("Creating the Cloudflare SDP answer failed: %s"), *Error));
						});
					});
			});
		},
		[WeakThis](const FString& Error)
		{
			DispatchToGameThread(WeakThis, [Error](FCloudflareRealtimeTransport& Self)
			{
				Self.ReportEndpointError(FString::Printf(
					TEXT("Applying the Cloudflare SDP offer failed: %s"), *Error));
			});
		});
}

void FCloudflareRealtimeTransport::SendRealtimeAnswer(
	const webrtc::SessionDescriptionInterface* Answer)
{
	const FString Sdp = SerializeSdp(Answer);
	if (Sdp.IsEmpty())
	{
		ReportEndpointError(TEXT("Could not serialize the Cloudflare SDP answer"));
		return;
	}
	const TSharedRef<FJsonObject> Description = MakeShared<FJsonObject>();
	Description->SetStringField(TEXT("type"), TEXT("answer"));
	Description->SetStringField(TEXT("sdp"), Sdp);
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetObjectField(TEXT("sessionDescription"), Description);
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	SendWorkerRequest(TEXT("/realtime/renegotiate"), TEXT("PUT"), Body,
		[WeakThis](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded)
		{
			DispatchToGameThread(WeakThis,
				[Response, bSucceeded](FCloudflareRealtimeTransport& Self)
			{
				if (Self.bClosing) return;
				TSharedPtr<FJsonObject> Json;
				FString Error;
				if (!Self.ParseJsonResponse(Response, bSucceeded, { 200 }, Json, Error))
				{
					Self.ReportEndpointError(Error);
					return;
				}
				Self.bDataTransportEstablished = true;
				UE_LOG(LogCloudflareRealtime, Display,
					TEXT("WEBRTC4UNREAL_CLOUDFLARE_SDP_ESTABLISHED role=%s room=%s"),
					Self.bIsHost ? TEXT("host") : TEXT("client"), *Self.Context->RoomId);
				Self.MarkTransportReady();
			});
		});
}

void FCloudflareRealtimeTransport::MarkTransportReady()
{
	if (bReadyRequestStarted || !bIceConnected || !bDataTransportEstablished) return;
	bReadyRequestStarted = true;
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	SendWorkerRequest(TEXT("/ready"), TEXT("POST"), MakeShared<FJsonObject>(),
		[WeakThis](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded)
		{
			DispatchToGameThread(WeakThis,
				[Response, bSucceeded](FCloudflareRealtimeTransport& Self)
			{
				if (Self.bClosing) return;
				TSharedPtr<FJsonObject> Json;
				FString Error;
				if (!Self.ParseJsonResponse(Response, bSucceeded, { 200 }, Json, Error))
				{
					Self.bReadyRequestStarted = false;
					Self.ReportEndpointError(Error);
					return;
				}
				UE_LOG(LogCloudflareRealtime, Display,
					TEXT("WEBRTC4UNREAL_CLOUDFLARE_TRANSPORT_READY role=%s room=%s"),
					Self.bIsHost ? TEXT("host") : TEXT("client"), *Self.Context->RoomId);
				Self.ProcessReadyPeers();
			});
		});
}

void FCloudflareRealtimeTransport::OnIceStateChanged(
	webrtc::PeerConnectionInterface::IceConnectionState NewState)
{
	if (NewState == webrtc::PeerConnectionInterface::kIceConnectionConnected
		|| NewState == webrtc::PeerConnectionInterface::kIceConnectionCompleted)
	{
		if (!bIceConnected)
		{
			bIceConnected = true;
			UE_LOG(LogCloudflareRealtime, Display,
				TEXT("WEBRTC4UNREAL_CLOUDFLARE_ICE_CONNECTED role=%s room=%s route=sfu"),
				bIsHost ? TEXT("host") : TEXT("client"), *Context->RoomId);
		}
		MarkTransportReady();
		ProcessReadyPeers();
	}
	else if (NewState == webrtc::PeerConnectionInterface::kIceConnectionFailed)
	{
		ReportEndpointError(TEXT("WebRTC ICE connection to Cloudflare Realtime failed"));
	}
}

void FCloudflareRealtimeTransport::ProcessReadyPeers()
{
	if (!bIceConnected || !bDataTransportEstablished || !bReadyRequestStarted) return;
	if (bIsHost)
	{
		TArray<FString> PeerIds;
		Peers.GenerateKeyArray(PeerIds);
		for (const FString& PeerId : PeerIds)
		{
			if (const TSharedPtr<FPeerState> Peer = FindPeer(PeerId))
			{
				PublishHostChannel(PeerId, Peer.ToSharedRef());
			}
		}
	}
	else
	{
		SubscribeClientChannel();
		PublishClientChannel();
	}
}

void FCloudflareRealtimeTransport::QueueHostPeer(const FString& PeerId,
	const FString& RemoteSessionId)
{
	if (!bIsHost || PeerId.IsEmpty() || PeerId == Context->ParticipantId
		|| RemoteSessionId.IsEmpty()) return;
	const TSharedRef<FPeerState> Peer = FindOrAddPeer(PeerId);
	Peer->RemoteSessionId = RemoteSessionId;
	ProcessReadyPeers();
}

void FCloudflareRealtimeTransport::PublishHostChannel(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer)
{
	if (!bIsHost || !bIceConnected || Peer->RemoteSessionId.IsEmpty()
		|| Peer->bPublishRequested || Peer->SendDataChannel) return;
	Peer->bPublishRequested = true;
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("peer_id"), PeerId);
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	SendWorkerRequest(TEXT("/realtime/publish"), TEXT("POST"), Body,
		[WeakThis, PeerId](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded)
		{
			DispatchToGameThread(WeakThis,
				[PeerId, Response, bSucceeded](FCloudflareRealtimeTransport& Self)
			{
				if (Self.bClosing) return;
				const TSharedPtr<FPeerState> Current = Self.FindPeer(PeerId);
				if (!Current) return;
				TSharedPtr<FJsonObject> Json;
				FString Error;
				if (!Self.ParseJsonResponse(Response, bSucceeded, { 200 }, Json, Error))
				{
					Current->bPublishRequested = false;
					Self.ReportPeerError(PeerId, Error);
					return;
				}
				const int32 ChannelId = ReadDataChannelId(Json);
				if (ChannelId < 0 || ChannelId > 65534 || !Self.PeerConnection)
				{
					Self.ReportPeerError(PeerId, TEXT("Cloudflare returned an invalid publisher DataChannel ID"));
					return;
				}
				Self.BindSendDataChannel(PeerId, Current.ToSharedRef(),
					Self.PeerConnection->CreateDataChannel(ChannelId, true));
				const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
				Frame->SetStringField(TEXT("type"), TEXT("channel_offer"));
				Frame->SetStringField(TEXT("target_peer_id"), PeerId);
				if (!Self.SendControl(Frame))
				{
					Self.ReportPeerError(PeerId, TEXT("Could not send the Cloudflare channel offer"));
				}
			});
		});
}

void FCloudflareRealtimeTransport::SubscribeHostChannel(const FString& PeerId,
	const FString& PublisherSessionId, const FString& DataChannelName)
{
	if (!bIsHost || !bIceConnected || PeerId.IsEmpty()
		|| PublisherSessionId.IsEmpty() || DataChannelName != TEXT("ue-up-") + PeerId)
	{
		ReportPeerError(PeerId, TEXT("Cloudflare returned invalid client uplink metadata"));
		return;
	}
	const TSharedPtr<FPeerState> Peer = FindPeer(PeerId);
	if (!Peer || Peer->RemoteSessionId != PublisherSessionId)
	{
		ReportPeerError(PeerId, TEXT("Cloudflare client uplink session did not match the room participant"));
		return;
	}
	if (Peer->bUplinkSubscribeRequested || Peer->ReceiveDataChannel) return;
	Peer->bUplinkSubscribeRequested = true;
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("peer_id"), PeerId);
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	SendWorkerRequest(TEXT("/realtime/subscribe-upstream"), TEXT("POST"), Body,
		[WeakThis, PeerId](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded)
		{
			DispatchToGameThread(WeakThis,
				[PeerId, Response, bSucceeded](FCloudflareRealtimeTransport& Self)
			{
				if (Self.bClosing) return;
				const TSharedPtr<FPeerState> Current = Self.FindPeer(PeerId);
				if (!Current) return;
				TSharedPtr<FJsonObject> Json;
				FString Error;
				if (!Self.ParseJsonResponse(Response, bSucceeded, { 200 }, Json, Error))
				{
					Current->bUplinkSubscribeRequested = false;
					Self.ReportPeerError(PeerId, Error);
					return;
				}
				const int32 ChannelId = ReadDataChannelId(Json);
				if (ChannelId < 0 || ChannelId > 65534 || !Self.PeerConnection)
				{
					Self.ReportPeerError(PeerId,
						TEXT("Cloudflare returned an invalid uplink subscriber DataChannel ID"));
					return;
				}
				Self.BindReceiveDataChannel(PeerId, Current.ToSharedRef(),
					Self.PeerConnection->CreateDataChannel(ChannelId, true));
			});
		});
}

void FCloudflareRealtimeTransport::QueueClientChannel(
	const FString& PublisherSessionId, const FString& DataChannelName)
{
	if (bIsHost || PublisherSessionId.IsEmpty()
		|| DataChannelName != TEXT("ue-down-") + Context->ParticipantId)
	{
		ReportEndpointError(TEXT("Cloudflare returned invalid listen-server channel metadata"));
		return;
	}
	PendingPublisherSessionId = PublisherSessionId;
	PendingDataChannelName = DataChannelName;
	const TSharedRef<FPeerState> HostPeer = FindOrAddPeer(Context->HostId);
	HostPeer->RemoteSessionId = PublisherSessionId;
	ProcessReadyPeers();
}

void FCloudflareRealtimeTransport::SubscribeClientChannel()
{
	if (bIsHost || !bIceConnected || PendingPublisherSessionId.IsEmpty()) return;
	const TSharedRef<FPeerState> HostPeer = FindOrAddPeer(Context->HostId);
	if (HostPeer->bSubscribeRequested || HostPeer->ReceiveDataChannel) return;
	HostPeer->bSubscribeRequested = true;
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	SendWorkerRequest(TEXT("/realtime/subscribe"), TEXT("POST"), MakeShared<FJsonObject>(),
		[WeakThis](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded)
		{
			DispatchToGameThread(WeakThis,
				[Response, bSucceeded](FCloudflareRealtimeTransport& Self)
			{
				if (Self.bClosing) return;
				const TSharedPtr<FPeerState> Current = Self.FindPeer(Self.Context->HostId);
				if (!Current) return;
				TSharedPtr<FJsonObject> Json;
				FString Error;
				if (!Self.ParseJsonResponse(Response, bSucceeded, { 200 }, Json, Error))
				{
					Current->bSubscribeRequested = false;
					Self.ReportPeerError(Self.Context->HostId, Error);
					return;
				}
				const int32 ChannelId = ReadDataChannelId(Json);
				if (ChannelId < 0 || ChannelId > 65534 || !Self.PeerConnection)
				{
					Self.ReportPeerError(Self.Context->HostId,
						TEXT("Cloudflare returned an invalid subscriber DataChannel ID"));
					return;
				}
				Self.BindReceiveDataChannel(Self.Context->HostId, Current.ToSharedRef(),
					Self.PeerConnection->CreateDataChannel(ChannelId, true));
			});
		});
}

void FCloudflareRealtimeTransport::PublishClientChannel()
{
	if (bIsHost || !bIceConnected || PendingPublisherSessionId.IsEmpty()) return;
	const TSharedRef<FPeerState> HostPeer = FindOrAddPeer(Context->HostId);
	if (HostPeer->bUplinkPublishRequested || HostPeer->SendDataChannel) return;
	HostPeer->bUplinkPublishRequested = true;
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	SendWorkerRequest(TEXT("/realtime/publish-upstream"), TEXT("POST"), MakeShared<FJsonObject>(),
		[WeakThis](TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded)
		{
			DispatchToGameThread(WeakThis,
				[Response, bSucceeded](FCloudflareRealtimeTransport& Self)
			{
				if (Self.bClosing) return;
				const TSharedPtr<FPeerState> Current = Self.FindPeer(Self.Context->HostId);
				if (!Current) return;
				TSharedPtr<FJsonObject> Json;
				FString Error;
				if (!Self.ParseJsonResponse(Response, bSucceeded, { 200 }, Json, Error))
				{
					Current->bUplinkPublishRequested = false;
					Self.ReportPeerError(Self.Context->HostId, Error);
					return;
				}
				const int32 ChannelId = ReadDataChannelId(Json);
				if (ChannelId < 0 || ChannelId > 65534 || !Self.PeerConnection)
				{
					Self.ReportPeerError(Self.Context->HostId,
						TEXT("Cloudflare returned an invalid uplink publisher DataChannel ID"));
					return;
				}
				Self.BindSendDataChannel(Self.Context->HostId, Current.ToSharedRef(),
					Self.PeerConnection->CreateDataChannel(ChannelId, true));
				if (!Current->bChannelAnswerSent)
				{
					const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
					Frame->SetStringField(TEXT("type"), TEXT("channel_answer"));
					if (!Self.SendControl(Frame))
					{
						Self.ReportPeerError(Self.Context->HostId,
							TEXT("Could not send the Cloudflare uplink answer"));
						return;
					}
					Current->bChannelAnswerSent = true;
				}
			});
		});
}

void FCloudflareRealtimeTransport::SendWorkerRequest(const FString& Suffix,
	const FString& Verb, const TSharedPtr<FJsonObject>& Body, FHttpCallback Callback)
{
	if (!Context)
	{
		Callback(nullptr, false);
		return;
	}
	const FString Prefix = FString::Printf(TEXT("/v1/rooms/%s/participants/%s"),
		*Context->RoomId, *Context->ParticipantId);
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		FHttpModule::Get().CreateRequest();
	const TSharedRef<FHttpCallback, ESPMode::ThreadSafe> SharedCallback =
		MakeShared<FHttpCallback, ESPMode::ThreadSafe>(MoveTemp(Callback));
	Request->SetURL(Context->WorkerUrl + Prefix + Suffix);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Cache-Control"), TEXT("no-store"));
	Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Context->ParticipantToken);
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

bool FCloudflareRealtimeTransport::ParseJsonResponse(
	const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
	bool bTransportSucceeded, const TArray<int32>& ExpectedCodes,
	TSharedPtr<FJsonObject>& OutJson, FString& OutError) const
{
	if (!bTransportSucceeded || !Response)
	{
		OutError = TEXT("Cloudflare Realtime Worker could not be reached");
		return false;
	}
	if (!ExpectedCodes.Contains(Response->GetResponseCode()))
	{
		OutError = FString::Printf(TEXT("Cloudflare Realtime Worker failed with HTTP %d"),
			Response->GetResponseCode());
		TSharedPtr<FJsonObject> ErrorEnvelope;
		const TSharedRef<TJsonReader<>> ErrorReader =
			TJsonReaderFactory<>::Create(Response->GetContentAsString());
		if (FJsonSerializer::Deserialize(ErrorReader, ErrorEnvelope) && ErrorEnvelope
			&& ErrorEnvelope->HasTypedField<EJson::Object>(TEXT("error")))
		{
			const TSharedPtr<FJsonObject> ApiError = ErrorEnvelope->GetObjectField(TEXT("error"));
			const FString Message = GetString(ApiError, TEXT("message"));
			if (!Message.IsEmpty()) OutError = Message;
		}
		return false;
	}
	if (Response->GetContentAsString().IsEmpty())
	{
		OutJson = MakeShared<FJsonObject>();
		return true;
	}
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, OutJson) || !OutJson)
	{
		OutError = TEXT("Cloudflare Realtime Worker returned invalid JSON");
		return false;
	}
	return true;
}

int32 FCloudflareRealtimeTransport::ReadDataChannelId(const TSharedPtr<FJsonObject>& Json)
{
	const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
	if ((!Json->TryGetArrayField(TEXT("dataChannels"), Channels) || !Channels)
		&& (!Json->TryGetArrayField(TEXT("datachannels"), Channels) || !Channels))
	{
		return -1;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Channels)
	{
		const TSharedPtr<FJsonObject>* Channel = nullptr;
		if (!Value || !Value->TryGetObject(Channel) || !Channel || !*Channel) continue;
		double Id = -1.0;
		if ((*Channel)->TryGetNumberField(TEXT("id"), Id)) return static_cast<int32>(Id);
	}
	return -1;
}

FString FCloudflareRealtimeTransport::GetString(
	const TSharedPtr<FJsonObject>& Json, const TCHAR* FieldName)
{
	FString Value;
	if (Json) Json->TryGetStringField(FieldName, Value);
	return Value;
}

TSharedRef<FCloudflareRealtimeTransport::FPeerState>
FCloudflareRealtimeTransport::FindOrAddPeer(const FString& PeerId)
{
	if (const TSharedPtr<FPeerState>* Existing = Peers.Find(PeerId))
	{
		return Existing->ToSharedRef();
	}
	const TSharedRef<FPeerState> Peer = MakeShared<FPeerState>();
	Peers.Add(PeerId, Peer);
	return Peer;
}

TSharedPtr<FCloudflareRealtimeTransport::FPeerState>
FCloudflareRealtimeTransport::FindPeer(const FString& PeerId) const
{
	const TSharedPtr<FPeerState>* Found = Peers.Find(PeerId);
	return Found ? *Found : nullptr;
}

void FCloudflareRealtimeTransport::BindSendDataChannel(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer,
	const TSharedPtr<FPixelStreamingDataChannel>& Channel)
{
	if (!Channel)
	{
		ReportPeerError(PeerId, TEXT("WebRTC could not create the negotiated send DataChannel"));
		return;
	}
	Peer->SendDataChannel = Channel;
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	Channel->OnOpen.AddLambda([WeakThis, PeerId](FPixelStreamingDataChannel&)
	{
		DispatchToGameThread(WeakThis, [PeerId](FCloudflareRealtimeTransport& Self)
		{
			if (!Self.bClosing) Self.OnDataChannelOpen(PeerId, true);
		});
	});
	Channel->OnClosed.AddLambda([WeakThis, PeerId](FPixelStreamingDataChannel&)
	{
		DispatchToGameThread(WeakThis, [PeerId](FCloudflareRealtimeTransport& Self)
		{
			if (const TSharedPtr<FPeerState> Current = Self.FindPeer(PeerId))
			{
				Self.Peers.Remove(PeerId);
				Current->bSendDataChannelOpen = false;
				Current->bReceiveDataChannelOpen = false;
				Current->SendDataChannel.Reset();
				Current->ReceiveDataChannel.Reset();
				Self.NotifyPeerDisconnected(PeerId, Current.ToSharedRef());
			}
		});
	});
	// A negotiated channel on an established SCTP association can open before the
	// delegate above is attached. A harmless control byte doubles as an open-state probe.
	if (Channel->SendMessage(CloudflareAckMessageType))
	{
		OnDataChannelOpen(PeerId, true);
	}
}

void FCloudflareRealtimeTransport::BindReceiveDataChannel(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer,
	const TSharedPtr<FPixelStreamingDataChannel>& Channel)
{
	if (!Channel)
	{
		ReportPeerError(PeerId, TEXT("WebRTC could not create the negotiated receive DataChannel"));
		return;
	}
	Peer->ReceiveDataChannel = Channel;
	TWeakPtr<FCloudflareRealtimeTransport> WeakThis = AsShared();
	Channel->OnOpen.AddLambda([WeakThis, PeerId](FPixelStreamingDataChannel&)
	{
		DispatchToGameThread(WeakThis, [PeerId](FCloudflareRealtimeTransport& Self)
		{
			if (!Self.bClosing) Self.OnDataChannelOpen(PeerId, false);
		});
	});
	Channel->OnClosed.AddLambda([WeakThis, PeerId](FPixelStreamingDataChannel&)
	{
		DispatchToGameThread(WeakThis, [PeerId](FCloudflareRealtimeTransport& Self)
		{
			if (const TSharedPtr<FPeerState> Current = Self.FindPeer(PeerId))
			{
				Self.Peers.Remove(PeerId);
				Current->bSendDataChannelOpen = false;
				Current->bReceiveDataChannelOpen = false;
				Current->SendDataChannel.Reset();
				Current->ReceiveDataChannel.Reset();
				Self.NotifyPeerDisconnected(PeerId, Current.ToSharedRef());
			}
		});
	});
	Channel->OnMessageReceived.AddLambda(
		[WeakThis, PeerId](uint8 Type, const webrtc::DataBuffer& Buffer)
		{
			if (const TSharedPtr<FCloudflareRealtimeTransport> Self = WeakThis.Pin())
			{
				Self->HandleChannelMessage(PeerId, Type, Buffer);
			}
		});
	if (Channel->SendMessage(CloudflareAckMessageType))
	{
		OnDataChannelOpen(PeerId, false);
	}
}

void FCloudflareRealtimeTransport::OnDataChannelOpen(const FString& PeerId,
	bool bSendChannel)
{
	const TSharedPtr<FPeerState> Peer = FindPeer(PeerId);
	if (!Peer) return;
	bool& bOpen = bSendChannel
		? Peer->bSendDataChannelOpen
		: Peer->bReceiveDataChannelOpen;
	if (bOpen) return;
	bOpen = true;
	UE_LOG(LogCloudflareRealtime, Log,
		TEXT("WEBRTC4UNREAL_CLOUDFLARE_STREAM_OPEN role=%s direction=%s peer=%s room=%s"),
		bIsHost ? TEXT("host") : TEXT("client"),
		bSendChannel ? TEXT("send") : TEXT("receive"), *PeerId, *Context->RoomId);
	if (Peer->bSendDataChannelOpen && Peer->bReceiveDataChannelOpen
		&& !Peer->bLocalChannelsOpenLogged)
	{
		Peer->bLocalChannelsOpenLogged = true;
		UE_LOG(LogCloudflareRealtime, Display,
			TEXT("WEBRTC4UNREAL_CLOUDFLARE_DATACHANNEL_OPEN role=%s peer=%s room=%s route=sfu streams=2"),
			bIsHost ? TEXT("host") : TEXT("client"), *PeerId, *Context->RoomId);
	}
	MaybeNotifyPeerConnected(PeerId, Peer.ToSharedRef());
}

void FCloudflareRealtimeTransport::MaybeNotifyPeerConnected(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer)
{
	if (!Peer->bSendDataChannelOpen || !Peer->bReceiveDataChannelOpen) return;
	if (!Peer->bLocalReadySent && (!bIsHost || Peer->bRemoteReady))
	{
		const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("type"), TEXT("channel_ready"));
		if (bIsHost) Frame->SetStringField(TEXT("target_peer_id"), PeerId);
		if (!SendControl(Frame))
		{
			ReportPeerError(PeerId, TEXT("Could not confirm the duplex Cloudflare channels"));
			return;
		}
		Peer->bLocalReadySent = true;
	}
	if (Peer->bConnectedNotified || !Peer->bLocalReadySent || !Peer->bRemoteReady) return;
	Peer->bConnectedNotified = true;
	UE_LOG(LogCloudflareRealtime, Display,
		TEXT("WEBRTC4UNREAL_CLOUDFLARE_PEER_CONNECTED role=%s peer=%s room=%s"),
		bIsHost ? TEXT("host") : TEXT("client"), *PeerId, *Context->RoomId);
	if (Callbacks.OnPeerConnected) Callbacks.OnPeerConnected(PeerId);
}

void FCloudflareRealtimeTransport::HandleChannelMessage(const FString& PeerId,
	uint8 Type, const webrtc::DataBuffer& Buffer)
{
	constexpr int32 HeaderSize = sizeof(uint8) + sizeof(int32);
	if (Type != UnrealPacketMessageType || Buffer.data.size() < HeaderSize) return;
	const uint8* Bytes = Buffer.data.data();
	int32 PayloadSize = 0;
	FMemory::Memcpy(&PayloadSize, Bytes + sizeof(uint8), sizeof(PayloadSize));
	if (PayloadSize <= 0 || PayloadSize > static_cast<int32>(Buffer.data.size()) - HeaderSize)
	{
		ReportPeerError(PeerId, TEXT("Received a malformed Unreal packet through Cloudflare Realtime"));
		return;
	}
	TArray<uint8> Packet;
	Packet.Append(Bytes + HeaderSize, PayloadSize);
	if (Callbacks.OnPacket) Callbacks.OnPacket(PeerId, MoveTemp(Packet));
}

void FCloudflareRealtimeTransport::NotifyPeerDisconnected(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer)
{
	if (Peer->bDisconnectNotified) return;
	Peer->bDisconnectNotified = true;
	if (Callbacks.OnPeerDisconnected) Callbacks.OnPeerDisconnected(PeerId);
}

void FCloudflareRealtimeTransport::NotifyClosed()
{
	if (bClosedNotified) return;
	bClosedNotified = true;
	if (Callbacks.OnClosed) Callbacks.OnClosed();
}

void FCloudflareRealtimeTransport::ReportPeerError(const FString& PeerId,
	const FString& Error)
{
	UE_LOG(LogCloudflareRealtime, Error,
		TEXT("WEBRTC4UNREAL_CLOUDFLARE_PEER_ERROR peer=%s room=%s message=%s"),
		*PeerId, Context ? *Context->RoomId : TEXT(""), *Error);
	if (Callbacks.OnError) Callbacks.OnError(PeerId, Error);
}

void FCloudflareRealtimeTransport::ReportEndpointError(const FString& Error)
{
	UE_LOG(LogCloudflareRealtime, Error,
		TEXT("WEBRTC4UNREAL_CLOUDFLARE_TRANSPORT_ERROR room=%s message=%s"),
		Context ? *Context->RoomId : TEXT(""), *Error);
	if (Callbacks.OnError) Callbacks.OnError(TEXT(""), Error);
}
