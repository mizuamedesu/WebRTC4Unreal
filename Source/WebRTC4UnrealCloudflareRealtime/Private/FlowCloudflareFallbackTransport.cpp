#include "FlowCloudflareFallbackTransport.h"

#include "Async/Async.h"
#include "HAL/PlatformTime.h"
#include "WebRTC4UnrealTransportRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlowCloudflareFallback, Log, All);

namespace
{
	constexpr int32 MaxPendingPacketsPerQueue = 256;
	constexpr int32 MaxPendingBytesPerPeer = 2 * 1024 * 1024;
	constexpr double RecoveryTimeoutSeconds = 5.0;

	const TCHAR* PathName(bool bPrimary)
	{
		return bPrimary ? TEXT("flow") : TEXT("cloudflare");
	}
}

FFlowCloudflareFallbackTransport::~FFlowCloudflareFallbackTransport()
{
	Close();
}

bool FFlowCloudflareFallbackTransport::Start(
	const TSharedRef<IWebRTC4UnrealTransportContext>& InContext, bool bHost,
	FWebRTC4UnrealTransportCallbacks InCallbacks, FString& Error)
{
	Context = StaticCastSharedRef<FFlowCloudflareFallbackTransportContext>(InContext);
	if (!Context || Context->RoomId.IsEmpty()
		|| (!Context->Primary.IsAvailable() && !Context->Fallback.IsAvailable()))
	{
		Error = TEXT("Flow/Cloudflare fallback transport context is incomplete");
		Context.Reset();
		return false;
	}
	if (Context->bForceFallback && !Context->Fallback.IsAvailable())
	{
		Error = TEXT("Cloudflare fallback was forced but its context is unavailable");
		Context.Reset();
		return false;
	}

	bIsHost = bHost;
	bClosing = false;
	bClosedNotified = false;
	Callbacks = MoveTemp(InCallbacks);
	bPrimaryPathClosed = !Context->Primary.IsAvailable() || Context->bForceFallback;
	bFallbackPathClosed = !Context->Fallback.IsAvailable();

	FString PrimaryError;
	const bool bPrimaryStarted = !Context->bForceFallback
		&& Context->Primary.IsAvailable()
		&& StartPath(EPath::Primary, Context->Primary, bHost, PrimaryError);
	if (!bPrimaryStarted) bPrimaryPathClosed = true;

	FString FallbackError;
	const bool bFallbackStarted = Context->Fallback.IsAvailable()
		&& StartPath(EPath::Fallback, Context->Fallback, bHost, FallbackError);
	if (!bFallbackStarted) bFallbackPathClosed = true;

	if (!bPrimaryStarted && !bFallbackStarted)
	{
		Error = FString::Printf(TEXT("Flow failed to start (%s); Cloudflare failed to start (%s)"),
			PrimaryError.IsEmpty() ? TEXT("unavailable") : *PrimaryError,
			FallbackError.IsEmpty() ? TEXT("unavailable") : *FallbackError);
		PrimaryEndpoint.Reset();
		FallbackEndpoint.Reset();
		Context.Reset();
		return false;
	}

	TWeakPtr<FFlowCloudflareFallbackTransport> WeakThis = AsShared();
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakThis](float DeltaSeconds)
		{
			if (const TSharedPtr<FFlowCloudflareFallbackTransport> Self = WeakThis.Pin())
			{
				return Self->TickFallback(DeltaSeconds);
			}
			return false;
		}), 0.1f);

	UE_LOG(LogFlowCloudflareFallback, Display,
		TEXT("WEBRTC4UNREAL_HYBRID_TRANSPORT_STARTED role=%s room=%s flow=%d cloudflare=%d forced=%d timeout=%.1f"),
		bIsHost ? TEXT("host") : TEXT("client"), *Context->RoomId,
		bPrimaryStarted, bFallbackStarted, Context->bForceFallback,
		Context->FallbackTimeoutSeconds);
	return true;
}

bool FFlowCloudflareFallbackTransport::StartPath(EPath Path,
	const FFlowCloudflareFallbackPath& Definition, bool bHost, FString& Error)
{
	TSharedPtr<IWebRTC4UnrealTransportEndpoint> Endpoint =
		FWebRTC4UnrealTransportRegistry::CreateEndpoint(Definition.TransportName);
	if (!Endpoint || !Definition.Context)
	{
		Error = FString::Printf(TEXT("Transport factory '%s' is unavailable"),
			*Definition.TransportName.ToString());
		return false;
	}
	if (Path == EPath::Primary) PrimaryEndpoint = Endpoint;
	else FallbackEndpoint = Endpoint;

	if (!Endpoint->Start(Definition.Context.ToSharedRef(), bHost,
		MakePathCallbacks(Path), Error))
	{
		if (Path == EPath::Primary) PrimaryEndpoint.Reset();
		else FallbackEndpoint.Reset();
		return false;
	}
	return true;
}

FWebRTC4UnrealTransportCallbacks FFlowCloudflareFallbackTransport::MakePathCallbacks(EPath Path)
{
	FWebRTC4UnrealTransportCallbacks Result;
	TWeakPtr<FFlowCloudflareFallbackTransport> WeakThis = AsShared();
	Result.OnPeerConnected = [WeakThis, Path](const FString& PeerId)
	{
		auto Invoke = [WeakThis, Path, PeerId]()
		{
			if (const TSharedPtr<FFlowCloudflareFallbackTransport> Self = WeakThis.Pin())
			{
				Self->HandlePeerConnected(Path, PeerId);
			}
		};
		if (IsInGameThread()) Invoke();
		else AsyncTask(ENamedThreads::GameThread, MoveTemp(Invoke));
	};
	Result.OnPacket = [WeakThis, Path](const FString& PeerId, TArray<uint8>&& Packet)
	{
		auto Invoke = [WeakThis, Path, PeerId, Packet = MoveTemp(Packet)]() mutable
		{
			if (const TSharedPtr<FFlowCloudflareFallbackTransport> Self = WeakThis.Pin())
			{
				Self->HandlePacket(Path, PeerId, MoveTemp(Packet));
			}
		};
		if (IsInGameThread()) Invoke();
		else AsyncTask(ENamedThreads::GameThread, MoveTemp(Invoke));
	};
	Result.OnPeerDisconnected = [WeakThis, Path](const FString& PeerId)
	{
		auto Invoke = [WeakThis, Path, PeerId]()
		{
			if (const TSharedPtr<FFlowCloudflareFallbackTransport> Self = WeakThis.Pin())
			{
				Self->HandlePeerDisconnected(Path, PeerId);
			}
		};
		if (IsInGameThread()) Invoke();
		else AsyncTask(ENamedThreads::GameThread, MoveTemp(Invoke));
	};
	Result.OnError = [WeakThis, Path](const FString& PeerId, const FString& Error)
	{
		auto Invoke = [WeakThis, Path, PeerId, Error]()
		{
			if (const TSharedPtr<FFlowCloudflareFallbackTransport> Self = WeakThis.Pin())
			{
				Self->HandlePathError(Path, PeerId, Error);
			}
		};
		if (IsInGameThread()) Invoke();
		else AsyncTask(ENamedThreads::GameThread, MoveTemp(Invoke));
	};
	Result.OnClosed = [WeakThis, Path]()
	{
		auto Invoke = [WeakThis, Path]()
		{
			if (const TSharedPtr<FFlowCloudflareFallbackTransport> Self = WeakThis.Pin())
			{
				Self->HandlePathClosed(Path);
			}
		};
		if (IsInGameThread()) Invoke();
		else AsyncTask(ENamedThreads::GameThread, MoveTemp(Invoke));
	};
	return Result;
}

void FFlowCloudflareFallbackTransport::HandlePeerConnected(EPath Path,
	const FString& PeerId)
{
	if (bClosing || PeerId.IsEmpty()) return;
	FPeerState& Peer = Peers.FindOrAdd(PeerId);
	if (Path == EPath::Primary)
	{
		Peer.bPrimaryConnected = true;
		Peer.bPrimaryFailed = false;
		if (Peer.Selected == ESelectedPath::None)
		{
			SelectPath(PeerId, Peer, ESelectedPath::Primary, TEXT("flow_connected"));
		}
		else if (Peer.Selected == ESelectedPath::Fallback && Peer.bFallbackFailed)
		{
			SelectPath(PeerId, Peer, ESelectedPath::Primary, TEXT("flow_recovered"));
		}
		return;
	}

	Peer.bFallbackConnected = true;
	Peer.bFallbackFailed = false;
	if (Peer.FallbackReadySeconds <= 0.0)
	{
		Peer.FallbackReadySeconds = FPlatformTime::Seconds();
	}
	if (Peer.Selected == ESelectedPath::None
		&& (Context->bForceFallback || bPrimaryPathClosed || Peer.bPrimaryFailed))
	{
		SelectPath(PeerId, Peer, ESelectedPath::Fallback,
			Context->bForceFallback ? TEXT("forced") : TEXT("flow_unavailable"));
	}
	else if (Peer.Selected == ESelectedPath::Primary && Peer.bPrimaryFailed)
	{
		SelectPath(PeerId, Peer, ESelectedPath::Fallback, TEXT("flow_failed"));
	}
}

void FFlowCloudflareFallbackTransport::HandlePacket(EPath Path,
	const FString& PeerId, TArray<uint8>&& Packet)
{
	if (bClosing || PeerId.IsEmpty() || Packet.IsEmpty()) return;
	FPeerState& Peer = Peers.FindOrAdd(PeerId);
	const ESelectedPath IncomingPath = SelectedFor(Path);
	if (Path == EPath::Primary)
	{
		Peer.bPrimaryConnected = true;
		if (Peer.Selected == ESelectedPath::None)
		{
			SelectPath(PeerId, Peer, ESelectedPath::Primary, TEXT("flow_packet"));
		}
	}
	else
	{
		Peer.bFallbackConnected = true;
		if (Peer.Selected == ESelectedPath::None)
		{
			// Receiving traffic means the remote endpoint has already committed to fallback.
			SelectPath(PeerId, Peer, ESelectedPath::Fallback, TEXT("remote_fallback_packet"));
		}
	}

	if (Peer.Selected == IncomingPath)
	{
		if (Callbacks.OnPacket) Callbacks.OnPacket(PeerId, MoveTemp(Packet));
		return;
	}
	if (Peer.Selected == ESelectedPath::None)
	{
		TArray<TArray<uint8>>& Queue = Path == EPath::Primary
			? Peer.PendingPrimaryPackets : Peer.PendingFallbackPackets;
		if (!QueuePacket(Queue, Peer, MoveTemp(Packet)))
		{
			FailPeer(PeerId, TEXT("Hybrid incoming packet queue exceeded its limit"));
		}
	}
}

void FFlowCloudflareFallbackTransport::HandlePeerDisconnected(EPath Path,
	const FString& PeerId)
{
	if (bClosing || PeerId.IsEmpty()) return;
	FPeerState* Peer = Peers.Find(PeerId);
	if (!Peer) return;
	if (Path == EPath::Primary)
	{
		Peer->bPrimaryConnected = false;
		Peer->bPrimaryFailed = true;
	}
	else
	{
		Peer->bFallbackConnected = false;
		Peer->bFallbackFailed = true;
	}

	if (Peer->Selected == SelectedFor(Path))
	{
		const ESelectedPath Alternative = Path == EPath::Primary
			? ESelectedPath::Fallback : ESelectedPath::Primary;
		const bool bAlternativeConnected = Alternative == ESelectedPath::Primary
			? Peer->bPrimaryConnected : Peer->bFallbackConnected;
		if (bAlternativeConnected)
		{
			SelectPath(PeerId, *Peer, Alternative, TEXT("selected_path_disconnected"));
		}
		else
		{
			Peer->bAwaitingRecovery = true;
			Peer->RecoveryDeadlineSeconds = FPlatformTime::Seconds() + RecoveryTimeoutSeconds;
		}
	}
	else if (Peer->Selected == ESelectedPath::None
		&& Peer->bPrimaryFailed && Peer->bFallbackFailed)
	{
		FailPeer(PeerId, TEXT("Both Flow and Cloudflare paths disconnected"));
	}
}

void FFlowCloudflareFallbackTransport::HandlePathError(EPath Path,
	const FString& PeerId, const FString& Error)
{
	UE_LOG(LogFlowCloudflareFallback, Warning,
		TEXT("WEBRTC4UNREAL_HYBRID_PATH_ERROR path=%s peer=%s room=%s message=%s"),
		PathName(Path == EPath::Primary), *PeerId,
		Context ? *Context->RoomId : TEXT(""), *Error);
	if (!PeerId.IsEmpty())
	{
		HandlePeerDisconnected(Path, PeerId);
		return;
	}

	if (Path == EPath::Primary) bPrimaryPathClosed = true;
	else bFallbackPathClosed = true;
	TArray<FString> PeerIds;
	Peers.GetKeys(PeerIds);
	for (const FString& Id : PeerIds)
	{
		HandlePeerDisconnected(Path, Id);
	}
	if (bPrimaryPathClosed && bFallbackPathClosed && Peers.IsEmpty()
		&& Callbacks.OnError)
	{
		Callbacks.OnError(TEXT(""), TEXT("Both Flow and Cloudflare transports failed"));
	}
}

void FFlowCloudflareFallbackTransport::HandlePathClosed(EPath Path)
{
	if (Path == EPath::Primary) bPrimaryPathClosed = true;
	else bFallbackPathClosed = true;
	if (bClosing || !bPrimaryPathClosed || !bFallbackPathClosed || bClosedNotified) return;
	bClosedNotified = true;
	if (Callbacks.OnClosed) Callbacks.OnClosed();
}

void FFlowCloudflareFallbackTransport::SelectPath(const FString& PeerId,
	FPeerState& Peer, ESelectedPath Path, const TCHAR* Reason)
{
	if (Path == ESelectedPath::None || Peer.Selected == Path) return;
	const ESelectedPath Previous = Peer.Selected;
	Peer.Selected = Path;
	Peer.bAwaitingRecovery = false;
	Peer.RecoveryDeadlineSeconds = 0.0;
	UE_LOG(LogFlowCloudflareFallback, Display,
		TEXT("WEBRTC4UNREAL_HYBRID_PATH_%s peer=%s room=%s path=%s previous=%d reason=%s"),
		Peer.bParentConnectedNotified ? TEXT("SWITCHED") : TEXT("SELECTED"),
		*PeerId, Context ? *Context->RoomId : TEXT(""),
		Path == ESelectedPath::Primary ? TEXT("flow") : TEXT("cloudflare"),
		static_cast<int32>(Previous), Reason);
	if (!Peer.bParentConnectedNotified)
	{
		Peer.bParentConnectedNotified = true;
		if (Callbacks.OnPeerConnected) Callbacks.OnPeerConnected(PeerId);
	}
	FlushOutgoing(PeerId, Peer);
	FlushIncoming(PeerId, Peer);
}

void FFlowCloudflareFallbackTransport::FlushIncoming(const FString& PeerId,
	FPeerState& Peer)
{
	TArray<TArray<uint8>>& SelectedQueue = Peer.Selected == ESelectedPath::Primary
		? Peer.PendingPrimaryPackets : Peer.PendingFallbackPackets;
	TArray<TArray<uint8>>& DiscardedQueue = Peer.Selected == ESelectedPath::Primary
		? Peer.PendingFallbackPackets : Peer.PendingPrimaryPackets;
	for (const TArray<uint8>& Packet : DiscardedQueue) Peer.PendingBytes -= Packet.Num();
	DiscardedQueue.Reset();
	TArray<TArray<uint8>> Packets = MoveTemp(SelectedQueue);
	SelectedQueue.Reset();
	for (TArray<uint8>& Packet : Packets)
	{
		Peer.PendingBytes -= Packet.Num();
		if (Callbacks.OnPacket) Callbacks.OnPacket(PeerId, MoveTemp(Packet));
	}
	Peer.PendingBytes = FMath::Max(0, Peer.PendingBytes);
}

void FFlowCloudflareFallbackTransport::FlushOutgoing(const FString& PeerId,
	FPeerState& Peer)
{
	IWebRTC4UnrealTransportEndpoint* Endpoint = EndpointFor(Peer.Selected);
	if (!Endpoint || Peer.PendingOutgoingPackets.IsEmpty()) return;
	TArray<TArray<uint8>> Packets = MoveTemp(Peer.PendingOutgoingPackets);
	Peer.PendingOutgoingPackets.Reset();
	for (TArray<uint8>& Packet : Packets)
	{
		Peer.PendingBytes -= Packet.Num();
		if (!Endpoint->Send(PeerId, Packet.GetData(), Packet.Num()))
		{
			QueuePacket(Peer.PendingOutgoingPackets, Peer, MoveTemp(Packet));
		}
	}
	Peer.PendingBytes = FMath::Max(0, Peer.PendingBytes);
}

bool FFlowCloudflareFallbackTransport::QueuePacket(TArray<TArray<uint8>>& Queue,
	FPeerState& Peer, TArray<uint8>&& Packet)
{
	if (Packet.IsEmpty() || Queue.Num() >= MaxPendingPacketsPerQueue
		|| Peer.PendingBytes + Packet.Num() > MaxPendingBytesPerPeer)
	{
		return false;
	}
	Peer.PendingBytes += Packet.Num();
	Queue.Add(MoveTemp(Packet));
	return true;
}

bool FFlowCloudflareFallbackTransport::TickFallback(float DeltaSeconds)
{
	(void)DeltaSeconds;
	if (bClosing || !Context) return false;
	const double Now = FPlatformTime::Seconds();
	TArray<FString> FailedPeers;
	for (TPair<FString, FPeerState>& Pair : Peers)
	{
		FPeerState& Peer = Pair.Value;
		if (Peer.Selected == ESelectedPath::None && Peer.bFallbackConnected)
		{
			const bool bDeadlineReached = Peer.FallbackReadySeconds > 0.0
				&& Now - Peer.FallbackReadySeconds >= Context->FallbackTimeoutSeconds;
			if (Context->bForceFallback || bPrimaryPathClosed
				|| Peer.bPrimaryFailed || bDeadlineReached)
			{
				SelectPath(Pair.Key, Peer, ESelectedPath::Fallback,
					bDeadlineReached ? TEXT("flow_timeout") : TEXT("flow_unavailable"));
			}
		}
		if (Peer.bAwaitingRecovery)
		{
			const ESelectedPath Alternative = Peer.Selected == ESelectedPath::Primary
				? ESelectedPath::Fallback : ESelectedPath::Primary;
			const bool bAlternativeConnected = Alternative == ESelectedPath::Primary
				? Peer.bPrimaryConnected : Peer.bFallbackConnected;
			if (bAlternativeConnected)
			{
				SelectPath(Pair.Key, Peer, Alternative, TEXT("runtime_recovery"));
			}
			else if (Now >= Peer.RecoveryDeadlineSeconds)
			{
				FailedPeers.Add(Pair.Key);
			}
		}
	}
	for (const FString& PeerId : FailedPeers)
	{
		FailPeer(PeerId, TEXT("No transport recovered before the fallback deadline"));
	}
	return !bClosing;
}

bool FFlowCloudflareFallbackTransport::Send(const FString& PeerId,
	const uint8* Data, int32 NumBytes)
{
	if (bClosing || !Data || NumBytes <= 0) return false;
	FPeerState* Peer = Peers.Find(PeerId);
	if (!Peer) return false;
	IWebRTC4UnrealTransportEndpoint* Endpoint = EndpointFor(Peer->Selected);
	if (Endpoint && Endpoint->Send(PeerId, Data, NumBytes)) return true;

	const ESelectedPath Alternative = Peer->Selected == ESelectedPath::Primary
		? ESelectedPath::Fallback : ESelectedPath::Primary;
	const bool bAlternativeConnected = Alternative == ESelectedPath::Primary
		? Peer->bPrimaryConnected : Peer->bFallbackConnected;
	if (Peer->Selected != ESelectedPath::None && bAlternativeConnected)
	{
		SelectPath(PeerId, *Peer, Alternative, TEXT("selected_path_send_failed"));
		Endpoint = EndpointFor(Peer->Selected);
		if (Endpoint && Endpoint->Send(PeerId, Data, NumBytes)) return true;
	}

	TArray<uint8> Packet;
	Packet.Append(Data, NumBytes);
	return QueuePacket(Peer->PendingOutgoingPackets, *Peer, MoveTemp(Packet));
}

void FFlowCloudflareFallbackTransport::ClosePeer(const FString& PeerId)
{
	Peers.Remove(PeerId);
	if (PrimaryEndpoint) PrimaryEndpoint->ClosePeer(PeerId);
	if (FallbackEndpoint) FallbackEndpoint->ClosePeer(PeerId);
}

void FFlowCloudflareFallbackTransport::Close()
{
	if (bClosing) return;
	bClosing = true;
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	Peers.Reset();
	if (PrimaryEndpoint) PrimaryEndpoint->Close();
	if (FallbackEndpoint) FallbackEndpoint->Close();
	PrimaryEndpoint.Reset();
	FallbackEndpoint.Reset();
	bPrimaryPathClosed = true;
	bFallbackPathClosed = true;
	if (!bClosedNotified)
	{
		bClosedNotified = true;
		if (Callbacks.OnClosed) Callbacks.OnClosed();
	}
	Context.Reset();
}

bool FFlowCloudflareFallbackTransport::IsValid() const
{
	return !bClosing
		&& ((PrimaryEndpoint && PrimaryEndpoint->IsValid())
			|| (FallbackEndpoint && FallbackEndpoint->IsValid()));
}

void FFlowCloudflareFallbackTransport::FailPeer(const FString& PeerId,
	const FString& Error)
{
	FPeerState Peer;
	if (!Peers.RemoveAndCopyValue(PeerId, Peer)) return;
	if (PrimaryEndpoint) PrimaryEndpoint->ClosePeer(PeerId);
	if (FallbackEndpoint) FallbackEndpoint->ClosePeer(PeerId);
	if (Peer.bParentConnectedNotified)
	{
		if (Callbacks.OnPeerDisconnected) Callbacks.OnPeerDisconnected(PeerId);
	}
	else if (Callbacks.OnError)
	{
		Callbacks.OnError(PeerId, Error);
	}
}

IWebRTC4UnrealTransportEndpoint* FFlowCloudflareFallbackTransport::EndpointFor(
	ESelectedPath Path) const
{
	if (Path == ESelectedPath::Primary) return PrimaryEndpoint.Get();
	if (Path == ESelectedPath::Fallback) return FallbackEndpoint.Get();
	return nullptr;
}

FFlowCloudflareFallbackTransport::ESelectedPath
FFlowCloudflareFallbackTransport::SelectedFor(EPath Path)
{
	return Path == EPath::Primary ? ESelectedPath::Primary : ESelectedPath::Fallback;
}
