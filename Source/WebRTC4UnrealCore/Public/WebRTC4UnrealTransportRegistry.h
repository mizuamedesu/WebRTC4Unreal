#pragma once

#include "CoreMinimal.h"
#include "IWebRTC4UnrealTransport.h"

/**
 * In-memory handoff between a provider and the GameNetDriver created during
 * server/client travel. Secrets are never written to disk or command-line.
 */
struct WEBRTC4UNREALCORE_API FWebRTC4UnrealTransportRegistration
{
	FName TransportName;
	TSharedPtr<IWebRTC4UnrealTransportContext> Context;
	bool bHost = false;
};

class WEBRTC4UNREALCORE_API FWebRTC4UnrealTransportRegistry
{
public:
	using FFactory = TFunction<TSharedRef<IWebRTC4UnrealTransportEndpoint>()>;

	static void RegisterFactory(FName TransportName, FFactory Factory);
	static void UnregisterFactory(FName TransportName);
	static TSharedPtr<IWebRTC4UnrealTransportEndpoint> CreateEndpoint(FName TransportName);

	static bool RegisterContext(const FString& Key, FName TransportName,
		const TSharedRef<IWebRTC4UnrealTransportContext>& Context, bool bHost);
	static bool FindContext(const FString& Key, FWebRTC4UnrealTransportRegistration& OutRegistration);
	static void RemoveContext(const FString& Key);
};
