#pragma once

#include "CoreMinimal.h"

/** Opaque provider-owned data required to start one transport endpoint. */
class WEBRTC4UNREALCORE_API IWebRTC4UnrealTransportContext
{
public:
	virtual ~IWebRTC4UnrealTransportContext() = default;
};

/**
 * Transport callbacks may originate on a transport thread. The generic
 * NetDriver always marshals them onto Unreal's game thread before touching
 * UObjects.
 */
struct WEBRTC4UNREALCORE_API FWebRTC4UnrealTransportCallbacks
{
	TFunction<void(const FString& PeerId)> OnPeerConnected;
	TFunction<void(const FString& PeerId, TArray<uint8>&& Packet)> OnPacket;
	TFunction<void(const FString& PeerId)> OnPeerDisconnected;
	TFunction<void(const FString& PeerId, const FString& Error)> OnError;
	TFunction<void()> OnClosed;
};

/**
 * Provider-independent datagram endpoint. A listener may expose any number of
 * peers; every PeerId is mapped to a distinct Unreal UNetConnection.
 */
class WEBRTC4UNREALCORE_API IWebRTC4UnrealTransportEndpoint
{
public:
	virtual ~IWebRTC4UnrealTransportEndpoint() = default;

	virtual bool Start(const TSharedRef<IWebRTC4UnrealTransportContext>& Context, bool bHost,
		FWebRTC4UnrealTransportCallbacks Callbacks, FString& Error) = 0;
	virtual bool Send(const FString& PeerId, const uint8* Data, int32 NumBytes) = 0;
	virtual void ClosePeer(const FString& PeerId) = 0;
	virtual void Close() = 0;
	virtual bool IsValid() const = 0;
};
