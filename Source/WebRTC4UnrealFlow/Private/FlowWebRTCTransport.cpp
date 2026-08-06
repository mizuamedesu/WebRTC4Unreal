#include "FlowWebRTCTransport.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "PixelStreamingDataChannel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WebSocketsModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlowWebRTC, Log, All);

namespace
{
	constexpr uint8 UnrealPacketMessageType = 246;

	FString SdpError(const TCHAR* Operation, const FString& Error)
	{
		return FString::Printf(TEXT("%s failed: %s"), Operation, *Error);
	}

	FString GetFlowSignalString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object)
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	FString GetPeerPrincipal(const TSharedPtr<FJsonObject>& Frame)
	{
		const TSharedPtr<FJsonObject>* Peer = nullptr;
		if (Frame && Frame->TryGetObjectField(TEXT("peer"), Peer) && Peer && *Peer)
		{
			return GetFlowSignalString(*Peer, TEXT("principal_id"));
		}
		return FString();
	}

	template <typename CallableType>
	void DispatchToGameThread(TWeakPtr<FFlowWebRTCTransport> WeakTransport,
		CallableType&& Callable)
	{
		TFunction<void(FFlowWebRTCTransport&)> Operation(Forward<CallableType>(Callable));
		if (IsInGameThread())
		{
			if (const TSharedPtr<FFlowWebRTCTransport> Transport = WeakTransport.Pin())
			{
				Operation(*Transport);
			}
			return;
		}
		AsyncTask(ENamedThreads::GameThread,
			[WeakTransport, Operation = MoveTemp(Operation)]() mutable
			{
				if (const TSharedPtr<FFlowWebRTCTransport> Transport = WeakTransport.Pin())
				{
					Operation(*Transport);
				}
			});
	}
}

FFlowWebRTCTransport::FFlowWebRTCTransport() = default;

FFlowWebRTCTransport::~FFlowWebRTCTransport()
{
	Close();
}

bool FFlowWebRTCTransport::Start(
	const TSharedRef<IWebRTC4UnrealTransportContext>& InContext, bool bHost,
	FWebRTC4UnrealTransportCallbacks InCallbacks, FString& Error)
{
	Context = StaticCastSharedRef<FFlowWebRTCTransportContext>(InContext);
	if (!Context || Context->RoomId.IsEmpty() || Context->Protocol != TEXT("flow-signaling.v1")
		|| Context->SignallingUrls.IsEmpty() || Context->PrincipalContext.IsEmpty()
		|| Context->Timestamp.IsEmpty() || Context->Signature.IsEmpty()
		|| Context->HostPrincipalId.IsEmpty() || Context->IceServers.IsEmpty())
	{
		Error = TEXT("Flow transport context is incomplete");
		Context.Reset();
		return false;
	}

	bIsHost = bHost;
	bClosing = false;
	bClosedNotified = false;
	Callbacks = MoveTemp(InCallbacks);
	ConfigureIce();
	ConnectSignalling();
	if (!SignallingSocket)
	{
		Error = TEXT("Flow signalling WebSocket could not be created");
		return false;
	}
	return true;
}

bool FFlowWebRTCTransport::Send(const FString& PeerId, const uint8* Data, int32 NumBytes)
{
	const TSharedPtr<FPeerState> Peer = FindPeer(PeerId);
	if (!Peer || !Peer->bDataChannelOpen || !Peer->DataChannel || !Data || NumBytes <= 0)
	{
		return false;
	}
	TArray64<uint8> Packet;
	Packet.Append(Data, NumBytes);
	return Peer->DataChannel->SendArbitraryData(UnrealPacketMessageType, Packet);
}

void FFlowWebRTCTransport::ClosePeer(const FString& PeerId)
{
	const TSharedPtr<FPeerState> Peer = FindPeer(PeerId);
	if (!Peer)
	{
		return;
	}
	if (bSignallingAuthenticated && SignallingSocket && SignallingSocket->IsConnected())
	{
		SendSignal(PeerId, TEXT("leave"), MakeShared<FJsonObject>());
	}
	Peers.Remove(PeerId);
	Peer->bDataChannelOpen = false;
	Peer->DataChannel.Reset();
	Peer->PeerConnection.Reset();
	NotifyPeerDisconnected(PeerId, Peer.ToSharedRef());
}

void FFlowWebRTCTransport::Close()
{
	if (bClosing)
	{
		return;
	}
	bClosing = true;
	for (const TPair<FString, TSharedPtr<FPeerState>>& Pair : Peers)
	{
		if (Pair.Value)
		{
			Pair.Value->bDataChannelOpen = false;
			Pair.Value->DataChannel.Reset();
			Pair.Value->PeerConnection.Reset();
		}
	}
	Peers.Reset();

	if (SignallingSocket)
	{
		SignallingSocket->OnConnected().Remove(SignallingConnectedHandle);
		SignallingSocket->OnConnectionError().Remove(SignallingErrorHandle);
		SignallingSocket->OnClosed().Remove(SignallingClosedHandle);
		SignallingSocket->OnMessage().Remove(SignallingMessageHandle);
		SignallingSocket->Close(1000, TEXT("WebRTC4Unreal Flow transport closed"));
		SignallingSocket.Reset();
	}
	NotifyClosed();
}

bool FFlowWebRTCTransport::IsValid() const
{
	return !bClosing && SignallingSocket.IsValid();
}

void FFlowWebRTCTransport::ConfigureIce()
{
	IceConfig = webrtc::PeerConnectionInterface::RTCConfiguration();
	IceConfig.type = webrtc::PeerConnectionInterface::kAll;
	IceConfig.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
	for (const FFlowWebRTCIceServer& Source : Context->IceServers)
	{
		IceConfig.servers.push_back(webrtc::PeerConnectionInterface::IceServer{});
		webrtc::PeerConnectionInterface::IceServer& Destination = IceConfig.servers.back();
		for (const FString& Url : Source.Urls)
		{
			Destination.urls.push_back(TCHAR_TO_UTF8(*Url));
		}
		Destination.username = TCHAR_TO_UTF8(*Source.Username);
		Destination.password = TCHAR_TO_UTF8(*Source.Credential);
	}
	UE_LOG(LogFlowWebRTC, Display,
		TEXT("WEBRTC4UNREAL_FLOW_ICE_CONFIGURED servers=%d policy=all room=%s"),
		Context->IceServers.Num(), *Context->RoomId);
}

void FFlowWebRTCTransport::ConnectSignalling()
{
	FWebSocketsModule& WebSockets = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	SignallingSocket = WebSockets.CreateWebSocket(Context->SignallingUrls[0], TEXT(""));
	if (!SignallingSocket)
	{
		return;
	}
	TWeakPtr<FFlowWebRTCTransport> WeakThis = AsShared();
	SignallingConnectedHandle = SignallingSocket->OnConnected().AddLambda([WeakThis]()
	{
		DispatchToGameThread(WeakThis, [](FFlowWebRTCTransport& Self)
		{
			if (!Self.bClosing) Self.OnSignallingConnected();
		});
	});
	SignallingErrorHandle = SignallingSocket->OnConnectionError().AddLambda(
		[WeakThis](const FString& Error)
		{
			DispatchToGameThread(WeakThis, [Error](FFlowWebRTCTransport& Self)
			{
				if (!Self.bClosing) Self.OnSignallingConnectionError(Error);
			});
		});
	SignallingClosedHandle = SignallingSocket->OnClosed().AddLambda(
		[WeakThis](int32 StatusCode, const FString& Reason, bool bWasClean)
		{
			DispatchToGameThread(WeakThis,
				[StatusCode, Reason, bWasClean](FFlowWebRTCTransport& Self)
			{
				Self.OnSignallingClosed(StatusCode, Reason, bWasClean);
			});
		});
	SignallingMessageHandle = SignallingSocket->OnMessage().AddLambda(
		[WeakThis](const FString& Message)
		{
			DispatchToGameThread(WeakThis, [Message](FFlowWebRTCTransport& Self)
			{
				if (!Self.bClosing) Self.OnSignallingMessage(Message);
			});
		});
	UE_LOG(LogFlowWebRTC, Log,
		TEXT("WEBRTC4UNREAL_FLOW_SIGNAL_CONNECTING role=%s room=%s"),
		bIsHost ? TEXT("host") : TEXT("client"), *Context->RoomId);
	SignallingSocket->Connect();
}

void FFlowWebRTCTransport::OnSignallingConnected()
{
	const TSharedRef<FJsonObject> Authentication = MakeShared<FJsonObject>();
	Authentication->SetStringField(TEXT("type"), TEXT("signed_context"));
	Authentication->SetStringField(TEXT("principal_context"), Context->PrincipalContext);
	Authentication->SetStringField(TEXT("timestamp"), Context->Timestamp);
	Authentication->SetStringField(TEXT("signature"), Context->Signature);
	if (!SendJson(Authentication))
	{
		ReportEndpointError(TEXT("Could not send Flow signed_context authentication frame"));
	}
}

void FFlowWebRTCTransport::OnSignallingConnectionError(const FString& Error)
{
	ReportEndpointError(FString::Printf(TEXT("Flow signalling connection error: %s"), *Error));
}

void FFlowWebRTCTransport::OnSignallingClosed(int32 StatusCode, const FString& Reason,
	bool bWasClean)
{
	UE_LOG(LogFlowWebRTC, Warning,
		TEXT("Flow signalling closed status=%d clean=%d reason=%s"), StatusCode, bWasClean, *Reason);
	if (!bClosing)
	{
		ReportEndpointError(FString::Printf(
			TEXT("Flow signalling closed (HTTP/WebSocket %d): %s"), StatusCode, *Reason));
		NotifyClosed();
	}
}

void FFlowWebRTCTransport::OnSignallingMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Frame;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Frame) || !Frame)
	{
		ReportEndpointError(TEXT("Flow signalling returned a malformed JSON frame"));
		return;
	}
	const FString Type = GetFlowSignalString(Frame, TEXT("type"));
	if (Type == TEXT("authenticated")) HandleAuthenticated(Frame);
	else if (Type == TEXT("peer_joined")) HandlePeerJoined(Frame);
	else if (Type == TEXT("peer_left")) HandlePeerLeft(Frame);
	else if (Type == TEXT("signal")) HandleSignal(Frame);
	else if (Type == TEXT("error"))
	{
		ReportEndpointError(FString::Printf(TEXT("Flow signalling error %s: %s"),
			*GetFlowSignalString(Frame, TEXT("code")), *GetFlowSignalString(Frame, TEXT("message"))));
	}
}

void FFlowWebRTCTransport::HandleAuthenticated(const TSharedPtr<FJsonObject>& Frame)
{
	LocalPrincipalId = GetFlowSignalString(Frame, TEXT("principal_id"));
	if (LocalPrincipalId.IsEmpty()
		|| (!Context->LocalPrincipalId.IsEmpty() && LocalPrincipalId != Context->LocalPrincipalId))
	{
		ReportEndpointError(TEXT("Flow authenticated frame contained an unexpected principal_id"));
		return;
	}
	bSignallingAuthenticated = true;
	UE_LOG(LogFlowWebRTC, Display,
		TEXT("WEBRTC4UNREAL_FLOW_SIGNAL_AUTHENTICATED role=%s room=%s"),
		bIsHost ? TEXT("host") : TEXT("client"), *Context->RoomId);

	const TArray<TSharedPtr<FJsonValue>>* ExistingPeers = nullptr;
	if (Frame->TryGetArrayField(TEXT("peers"), ExistingPeers) && ExistingPeers)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ExistingPeers)
		{
			const TSharedPtr<FJsonObject>* PeerObject = nullptr;
			if (Value && Value->TryGetObject(PeerObject) && PeerObject && *PeerObject)
			{
				AddRemotePeer(GetFlowSignalString(*PeerObject, TEXT("principal_id")));
			}
		}
	}
}

void FFlowWebRTCTransport::HandlePeerJoined(const TSharedPtr<FJsonObject>& Frame)
{
	AddRemotePeer(GetPeerPrincipal(Frame));
}

void FFlowWebRTCTransport::HandlePeerLeft(const TSharedPtr<FJsonObject>& Frame)
{
	const FString PeerId = GetPeerPrincipal(Frame);
	if (const TSharedPtr<FPeerState> Peer = FindPeer(PeerId))
	{
		Peers.Remove(PeerId);
		Peer->bDataChannelOpen = false;
		Peer->DataChannel.Reset();
		Peer->PeerConnection.Reset();
		NotifyPeerDisconnected(PeerId, Peer.ToSharedRef());
	}
}

bool FFlowWebRTCTransport::IsAllowedPeer(const FString& PeerId) const
{
	if (PeerId.IsEmpty() || PeerId == LocalPrincipalId)
	{
		return false;
	}
	return bIsHost || PeerId == Context->HostPrincipalId;
}

bool FFlowWebRTCTransport::AddRemotePeer(const FString& PeerId)
{
	if (!IsAllowedPeer(PeerId))
	{
		return false;
	}
	TSharedPtr<FPeerState>& Peer = Peers.FindOrAdd(PeerId);
	if (!Peer)
	{
		Peer = MakeShared<FPeerState>();
	}
	if (!EnsurePeerConnection(PeerId, Peer.ToSharedRef()))
	{
		return false;
	}
	if (bIsHost)
	{
		BeginHostOffer(PeerId, Peer.ToSharedRef());
	}
	return true;
}

void FFlowWebRTCTransport::HandleSignal(const TSharedPtr<FJsonObject>& Frame)
{
	if (!bSignallingAuthenticated)
	{
		ReportEndpointError(TEXT("Flow signal arrived before authentication"));
		return;
	}
	const FString Sender = GetFlowSignalString(Frame, TEXT("sender"));
	if (!IsAllowedPeer(Sender) || !AddRemotePeer(Sender))
	{
		UE_LOG(LogFlowWebRTC, Warning, TEXT("Ignoring Flow signal from an unapproved peer"));
		return;
	}
	const TSharedPtr<FPeerState> Peer = FindPeer(Sender);
	const FString Kind = GetFlowSignalString(Frame, TEXT("kind"));
	const TSharedPtr<FJsonObject>* Payload = nullptr;
	if (!Peer || !Frame->TryGetObjectField(TEXT("payload"), Payload) || !Payload || !*Payload)
	{
		ReportPeerError(Sender, TEXT("Flow signal did not contain an object payload"));
		return;
	}
	if (Kind == TEXT("offer")) AcceptOffer(Sender, Peer.ToSharedRef(),
		GetFlowSignalString(*Payload, TEXT("sdp")));
	else if (Kind == TEXT("answer")) AcceptAnswer(Sender, Peer.ToSharedRef(),
		GetFlowSignalString(*Payload, TEXT("sdp")));
	else if (Kind == TEXT("ice_candidate"))
	{
		AddRemoteIceCandidate(Peer.ToSharedRef(), GetFlowSignalString(*Payload, TEXT("sdpMid")),
			static_cast<int32>((*Payload)->GetNumberField(TEXT("sdpMLineIndex"))),
			GetFlowSignalString(*Payload, TEXT("candidate")));
	}
	else if (Kind == TEXT("renegotiate") && bIsHost)
	{
		Peer->bOfferStarted = false;
		BeginHostOffer(Sender, Peer.ToSharedRef());
	}
	else if (Kind == TEXT("leave"))
	{
		Peers.Remove(Sender);
		Peer->bDataChannelOpen = false;
		Peer->DataChannel.Reset();
		Peer->PeerConnection.Reset();
		NotifyPeerDisconnected(Sender, Peer.ToSharedRef());
	}
}

bool FFlowWebRTCTransport::SendJson(const TSharedRef<FJsonObject>& Frame)
{
	if (!SignallingSocket || !SignallingSocket->IsConnected()) return false;
	FString Message;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Message);
	if (!FJsonSerializer::Serialize(Frame, Writer)) return false;
	SignallingSocket->Send(Message);
	return true;
}

bool FFlowWebRTCTransport::SendSignal(const FString& PeerId, const FString& Kind,
	const TSharedRef<FJsonObject>& Payload)
{
	if (!bSignallingAuthenticated || !IsAllowedPeer(PeerId)) return false;
	const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(TEXT("type"), Kind);
	Frame->SetStringField(TEXT("target"), PeerId);
	Frame->SetObjectField(TEXT("payload"), Payload);
	return SendJson(Frame);
}

void FFlowWebRTCTransport::SendSessionDescription(const FString& PeerId, const FString& Kind,
	const webrtc::SessionDescriptionInterface* Sdp)
{
	std::string SerializedSdp;
	if (!Sdp || !Sdp->ToString(&SerializedSdp))
	{
		ReportPeerError(PeerId, FString::Printf(TEXT("Could not serialize WebRTC %s"), *Kind));
		return;
	}
	const FString SdpText = UTF8_TO_TCHAR(SerializedSdp.c_str());
	DispatchToGameThread(AsShared(), [PeerId, Kind, SdpText](FFlowWebRTCTransport& Self)
	{
		if (Self.bClosing) return;
		const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("sdp"), SdpText);
		if (!Self.SendSignal(PeerId, Kind, Payload))
		{
			Self.ReportPeerError(PeerId,
				FString::Printf(TEXT("Could not send WebRTC %s"), *Kind));
		}
	});
}

void FFlowWebRTCTransport::SendIceCandidate(const FString& PeerId,
	const webrtc::IceCandidateInterface* Candidate)
{
	std::string CandidateText;
	if (!Candidate || !Candidate->ToString(&CandidateText))
	{
		ReportPeerError(PeerId, TEXT("Could not inspect a local ICE candidate"));
		return;
	}
	const FString CandidateString = UTF8_TO_TCHAR(CandidateText.c_str());
	const FString SdpMid = UTF8_TO_TCHAR(Candidate->sdp_mid().c_str());
	const int32 SdpMLineIndex = Candidate->sdp_mline_index();
	FString CandidateType(TEXT("unknown"));
	if (CandidateText.find(" typ host ") != std::string::npos) CandidateType = TEXT("host");
	else if (CandidateText.find(" typ srflx ") != std::string::npos) CandidateType = TEXT("srflx");
	else if (CandidateText.find(" typ prflx ") != std::string::npos) CandidateType = TEXT("prflx");
	else if (CandidateText.find(" typ relay ") != std::string::npos) CandidateType = TEXT("relay");

	DispatchToGameThread(AsShared(),
		[PeerId, CandidateString, SdpMid, SdpMLineIndex, CandidateType](
			FFlowWebRTCTransport& Self)
		{
			if (Self.bClosing) return;
			UE_LOG(LogFlowWebRTC, Display,
				TEXT("WEBRTC4UNREAL_FLOW_ICE_LOCAL_CANDIDATE peer=%s type=%s policy=all"),
				*PeerId, *CandidateType);
			const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("candidate"), CandidateString);
			Payload->SetNumberField(TEXT("sdpMLineIndex"), SdpMLineIndex);
			Payload->SetStringField(TEXT("sdpMid"), SdpMid);
			if (!Self.SendSignal(PeerId, TEXT("ice_candidate"), Payload))
			{
				Self.ReportPeerError(PeerId,
					TEXT("Could not send local ICE candidate"));
			}
		});
}

TSharedPtr<FFlowWebRTCTransport::FPeerState> FFlowWebRTCTransport::FindPeer(
	const FString& PeerId) const
{
	const TSharedPtr<FPeerState>* Found = Peers.Find(PeerId);
	return Found ? *Found : nullptr;
}

bool FFlowWebRTCTransport::EnsurePeerConnection(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer)
{
	if (Peer->PeerConnection) return true;
	Peer->PeerConnection = FPixelStreamingPeerConnection::Create(IceConfig);
	if (!Peer->PeerConnection)
	{
		ReportPeerError(PeerId, TEXT("Could not create native WebRTC peer connection"));
		return false;
	}
	TWeakPtr<FFlowWebRTCTransport> WeakThis = AsShared();
	Peer->PeerConnection->OnEmitIceCandidate.AddLambda(
		[WeakThis, PeerId](const webrtc::IceCandidateInterface* Candidate)
		{
			if (const TSharedPtr<FFlowWebRTCTransport> Self = WeakThis.Pin())
			{
				Self->SendIceCandidate(PeerId, Candidate);
			}
		});
	Peer->PeerConnection->OnNewDataChannel.AddLambda(
		[WeakThis, PeerId](TSharedPtr<FPixelStreamingDataChannel> Channel)
		{
			DispatchToGameThread(WeakThis,
				[PeerId, Channel](FFlowWebRTCTransport& Self)
			{
				if (Self.bClosing) return;
				if (const TSharedPtr<FPeerState> PinnedPeer = Self.FindPeer(PeerId))
				{
					Self.BindDataChannel(PeerId, PinnedPeer.ToSharedRef(), Channel);
				}
			});
		});
	Peer->PeerConnection->OnIceStateChanged.AddLambda(
		[WeakThis, PeerId](webrtc::PeerConnectionInterface::IceConnectionState NewState)
		{
			DispatchToGameThread(WeakThis,
				[PeerId, NewState](FFlowWebRTCTransport& Self)
			{
				if (Self.bClosing) return;
				if (NewState == webrtc::PeerConnectionInterface::kIceConnectionConnected
					|| NewState == webrtc::PeerConnectionInterface::kIceConnectionCompleted)
				{
					UE_LOG(LogFlowWebRTC, Display,
						TEXT("WEBRTC4UNREAL_FLOW_ICE_CONNECTED peer=%s room=%s"),
						*PeerId, *Self.Context->RoomId);
				}
				else if (NewState == webrtc::PeerConnectionInterface::kIceConnectionFailed)
				{
					Self.ReportPeerError(PeerId,
						TEXT("WebRTC ICE failed after direct and TURN fallback candidates"));
				}
			});
		});
	FlushPendingIceCandidates(Peer);
	return true;
}

void FFlowWebRTCTransport::BeginHostOffer(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer)
{
	if (!Peer->PeerConnection || Peer->bOfferStarted) return;
	Peer->bOfferStarted = true;
	if (!Peer->DataChannel)
	{
		BindDataChannel(PeerId, Peer, Peer->PeerConnection->CreateDataChannel(0, false));
	}
	TWeakPtr<FFlowWebRTCTransport> WeakThis = AsShared();
	Peer->PeerConnection->CreateOffer(FPixelStreamingPeerConnection::EReceiveMediaOption::Nothing,
		[WeakThis, PeerId](const webrtc::SessionDescriptionInterface* Sdp)
		{
			if (const TSharedPtr<FFlowWebRTCTransport> Self = WeakThis.Pin())
			{
				Self->SendSessionDescription(PeerId, TEXT("offer"), Sdp);
			}
		},
		[WeakThis, PeerId](const FString& Error)
		{
			DispatchToGameThread(WeakThis,
				[PeerId, Error](FFlowWebRTCTransport& Self)
			{
				if (const TSharedPtr<FPeerState> Current = Self.FindPeer(PeerId))
				{
					Current->bOfferStarted = false;
				}
				Self.ReportPeerError(PeerId, SdpError(TEXT("CreateOffer"), Error));
			});
		});
}

void FFlowWebRTCTransport::AcceptOffer(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer, const FString& Sdp)
{
	if (!Peer->PeerConnection || Sdp.IsEmpty())
	{
		ReportPeerError(PeerId, TEXT("Received an invalid WebRTC offer"));
		return;
	}
	TWeakPtr<FFlowWebRTCTransport> WeakThis = AsShared();
	Peer->PeerConnection->ReceiveOffer(Sdp,
		[WeakThis, PeerId]()
		{
			DispatchToGameThread(WeakThis, [WeakThis, PeerId](FFlowWebRTCTransport& Self)
			{
				if (Self.bClosing) return;
				if (const TSharedPtr<FPeerState> Current = Self.FindPeer(PeerId))
				{
					Current->PeerConnection->CreateAnswer(
						FPixelStreamingPeerConnection::EReceiveMediaOption::Nothing,
						[WeakThis, PeerId](const webrtc::SessionDescriptionInterface* Answer)
						{
							if (const TSharedPtr<FFlowWebRTCTransport> Pinned = WeakThis.Pin())
							{
								Pinned->SendSessionDescription(PeerId, TEXT("answer"), Answer);
							}
						},
						[WeakThis, PeerId](const FString& Error)
						{
							DispatchToGameThread(WeakThis,
								[PeerId, Error](FFlowWebRTCTransport& Pinned)
							{
								Pinned.ReportPeerError(PeerId,
									SdpError(TEXT("CreateAnswer"), Error));
							});
						});
				}
			});
		},
		[WeakThis, PeerId](const FString& Error)
		{
			DispatchToGameThread(WeakThis,
				[PeerId, Error](FFlowWebRTCTransport& Self)
			{
				Self.ReportPeerError(PeerId, SdpError(TEXT("ReceiveOffer"), Error));
			});
		});
}

void FFlowWebRTCTransport::AcceptAnswer(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer, const FString& Sdp)
{
	if (!Peer->PeerConnection || Sdp.IsEmpty())
	{
		ReportPeerError(PeerId, TEXT("Received an invalid WebRTC answer"));
		return;
	}
	TWeakPtr<FFlowWebRTCTransport> WeakThis = AsShared();
	Peer->PeerConnection->ReceiveAnswer(Sdp, []() {}, [WeakThis, PeerId](const FString& Error)
	{
		DispatchToGameThread(WeakThis, [PeerId, Error](FFlowWebRTCTransport& Self)
		{
			Self.ReportPeerError(PeerId, SdpError(TEXT("ReceiveAnswer"), Error));
		});
	});
}

void FFlowWebRTCTransport::AddRemoteIceCandidate(const TSharedRef<FPeerState>& Peer,
	const FString& SdpMid, int32 SdpMLineIndex, const FString& Candidate)
{
	if (Candidate.IsEmpty()) return;
	if (!Peer->PeerConnection)
	{
		Peer->PendingIceCandidates.Add({ SdpMid, SdpMLineIndex, Candidate });
		return;
	}
	Peer->PeerConnection->AddRemoteIceCandidate(SdpMid, SdpMLineIndex, Candidate);
}

void FFlowWebRTCTransport::FlushPendingIceCandidates(const TSharedRef<FPeerState>& Peer)
{
	if (!Peer->PeerConnection) return;
	for (const FPendingIceCandidate& Candidate : Peer->PendingIceCandidates)
	{
		Peer->PeerConnection->AddRemoteIceCandidate(Candidate.SdpMid,
			Candidate.SdpMLineIndex, Candidate.Candidate);
	}
	Peer->PendingIceCandidates.Reset();
}

void FFlowWebRTCTransport::BindDataChannel(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer, const TSharedPtr<FPixelStreamingDataChannel>& Channel)
{
	if (!Channel)
	{
		ReportPeerError(PeerId, TEXT("WebRTC did not create a data channel"));
		return;
	}
	if (Peer->DataChannel == Channel) return;
	Peer->DataChannel = Channel;
	TWeakPtr<FFlowWebRTCTransport> WeakThis = AsShared();
	Channel->OnOpen.AddLambda([WeakThis, PeerId](FPixelStreamingDataChannel&)
	{
		DispatchToGameThread(WeakThis, [PeerId](FFlowWebRTCTransport& Self)
		{
			if (Self.bClosing) return;
			if (const TSharedPtr<FPeerState> Current = Self.FindPeer(PeerId))
			{
				if (Current->bDataChannelOpen) return;
				Current->bDataChannelOpen = true;
				UE_LOG(LogFlowWebRTC, Display,
					TEXT("WEBRTC4UNREAL_FLOW_DATACHANNEL_OPEN peer=%s room=%s"),
					*PeerId, *Self.Context->RoomId);
				if (Self.Callbacks.OnPeerConnected) Self.Callbacks.OnPeerConnected(PeerId);
			}
		});
	});
	Channel->OnClosed.AddLambda([WeakThis, PeerId](FPixelStreamingDataChannel&)
	{
		DispatchToGameThread(WeakThis, [PeerId](FFlowWebRTCTransport& Self)
		{
			if (const TSharedPtr<FPeerState> Current = Self.FindPeer(PeerId))
			{
				Self.Peers.Remove(PeerId);
				Current->bDataChannelOpen = false;
				Current->DataChannel.Reset();
				Current->PeerConnection.Reset();
				Self.NotifyPeerDisconnected(PeerId, Current.ToSharedRef());
			}
		});
	});
	Channel->OnMessageReceived.AddLambda(
		[WeakThis, PeerId](uint8 Type, const webrtc::DataBuffer& Buffer)
		{
			if (const TSharedPtr<FFlowWebRTCTransport> Self = WeakThis.Pin())
			{
				Self->HandleChannelMessage(PeerId, Type, Buffer);
			}
		});
}

void FFlowWebRTCTransport::HandleChannelMessage(const FString& PeerId, uint8 Type,
	const webrtc::DataBuffer& Buffer)
{
	constexpr int32 HeaderSize = sizeof(uint8) + sizeof(int32);
	if (Type != UnrealPacketMessageType || Buffer.data.size() < HeaderSize) return;
	const uint8* Bytes = Buffer.data.data();
	int32 PayloadSize = 0;
	FMemory::Memcpy(&PayloadSize, Bytes + sizeof(uint8), sizeof(PayloadSize));
	if (PayloadSize <= 0 || PayloadSize > static_cast<int32>(Buffer.data.size()) - HeaderSize)
	{
		ReportPeerError(PeerId, TEXT("Received a malformed Unreal packet over WebRTC"));
		return;
	}
	TArray<uint8> Packet;
	Packet.Append(Bytes + HeaderSize, PayloadSize);
	if (Callbacks.OnPacket) Callbacks.OnPacket(PeerId, MoveTemp(Packet));
}

void FFlowWebRTCTransport::NotifyPeerDisconnected(const FString& PeerId,
	const TSharedRef<FPeerState>& Peer)
{
	if (Peer->bDisconnectNotified) return;
	Peer->bDisconnectNotified = true;
	if (Callbacks.OnPeerDisconnected) Callbacks.OnPeerDisconnected(PeerId);
}

void FFlowWebRTCTransport::NotifyClosed()
{
	if (bClosedNotified) return;
	bClosedNotified = true;
	if (Callbacks.OnClosed) Callbacks.OnClosed();
}

void FFlowWebRTCTransport::ReportPeerError(const FString& PeerId, const FString& Error)
{
	if (!IsInGameThread())
	{
		DispatchToGameThread(AsShared(),
			[PeerId, Error](FFlowWebRTCTransport& Self)
			{
				Self.ReportPeerError(PeerId, Error);
			});
		return;
	}
	UE_LOG(LogFlowWebRTC, Error, TEXT("WEBRTC4UNREAL_FLOW_PEER_ERROR peer=%s error=%s"),
		*PeerId, *Error);
	if (Callbacks.OnError) Callbacks.OnError(PeerId, Error);
}

void FFlowWebRTCTransport::ReportEndpointError(const FString& Error)
{
	if (!IsInGameThread())
	{
		DispatchToGameThread(AsShared(), [Error](FFlowWebRTCTransport& Self)
		{
			Self.ReportEndpointError(Error);
		});
		return;
	}
	UE_LOG(LogFlowWebRTC, Error, TEXT("WEBRTC4UNREAL_FLOW_TRANSPORT_ERROR %s"), *Error);
	if (Callbacks.OnError) Callbacks.OnError(FString(), Error);
}
