#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "IWebRTC4UnrealTransport.h"

struct FFlowCloudflareFallbackPath
{
	FName TransportName;
	TSharedPtr<IWebRTC4UnrealTransportContext> Context;

	bool IsAvailable() const
	{
		return !TransportName.IsNone() && Context.IsValid();
	}
};

/** Provider-neutral pair of transport contexts; Flow remains the preferred path. */
struct FFlowCloudflareFallbackTransportContext final : public IWebRTC4UnrealTransportContext
{
	FString RoomId;
	FFlowCloudflareFallbackPath Primary;
	FFlowCloudflareFallbackPath Fallback;
	double FallbackTimeoutSeconds = 8.0;
	bool bForceFallback = false;
};

/**
 * Runs Flow and Cloudflare Direct together, selecting Flow as soon as it opens.
 * Cloudflare is selected after the per-peer deadline or immediately when Flow fails.
 */
class FFlowCloudflareFallbackTransport final : public IWebRTC4UnrealTransportEndpoint,
	public TSharedFromThis<FFlowCloudflareFallbackTransport>
{
public:
	virtual ~FFlowCloudflareFallbackTransport() override;

	virtual bool Start(const TSharedRef<IWebRTC4UnrealTransportContext>& InContext, bool bHost,
		FWebRTC4UnrealTransportCallbacks InCallbacks, FString& Error) override;
	virtual bool Send(const FString& PeerId, const uint8* Data, int32 NumBytes) override;
	virtual void ClosePeer(const FString& PeerId) override;
	virtual void Close() override;
	virtual bool IsValid() const override;

private:
	enum class EPath : uint8
	{
		Primary,
		Fallback
	};

	enum class ESelectedPath : uint8
	{
		None,
		Primary,
		Fallback
	};

	struct FPeerState
	{
		ESelectedPath Selected = ESelectedPath::None;
		bool bPrimaryConnected = false;
		bool bFallbackConnected = false;
		bool bPrimaryFailed = false;
		bool bFallbackFailed = false;
		bool bParentConnectedNotified = false;
		bool bAwaitingRecovery = false;
		double FallbackReadySeconds = 0.0;
		double RecoveryDeadlineSeconds = 0.0;
		TArray<TArray<uint8>> PendingPrimaryPackets;
		TArray<TArray<uint8>> PendingFallbackPackets;
		TArray<TArray<uint8>> PendingOutgoingPackets;
		int32 PendingBytes = 0;
	};

	bool StartPath(EPath Path, const FFlowCloudflareFallbackPath& Definition, bool bHost,
		FString& Error);
	FWebRTC4UnrealTransportCallbacks MakePathCallbacks(EPath Path);
	void HandlePeerConnected(EPath Path, const FString& PeerId);
	void HandlePacket(EPath Path, const FString& PeerId, TArray<uint8>&& Packet);
	void HandlePeerDisconnected(EPath Path, const FString& PeerId);
	void HandlePathError(EPath Path, const FString& PeerId, const FString& Error);
	void HandlePathClosed(EPath Path);
	void SelectPath(const FString& PeerId, FPeerState& Peer, ESelectedPath Path,
		const TCHAR* Reason);
	void FlushIncoming(const FString& PeerId, FPeerState& Peer);
	void FlushOutgoing(const FString& PeerId, FPeerState& Peer);
	bool QueuePacket(TArray<TArray<uint8>>& Queue, FPeerState& Peer, TArray<uint8>&& Packet);
	bool TickFallback(float DeltaSeconds);
	void FailPeer(const FString& PeerId, const FString& Error);
	IWebRTC4UnrealTransportEndpoint* EndpointFor(ESelectedPath Path) const;
	static ESelectedPath SelectedFor(EPath Path);

	TSharedPtr<FFlowCloudflareFallbackTransportContext> Context;
	TSharedPtr<IWebRTC4UnrealTransportEndpoint> PrimaryEndpoint;
	TSharedPtr<IWebRTC4UnrealTransportEndpoint> FallbackEndpoint;
	FWebRTC4UnrealTransportCallbacks Callbacks;
	TMap<FString, FPeerState> Peers;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bIsHost = false;
	bool bClosing = false;
	bool bPrimaryPathClosed = false;
	bool bFallbackPathClosed = false;
	bool bClosedNotified = false;
};
