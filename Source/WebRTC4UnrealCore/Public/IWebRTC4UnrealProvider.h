#pragma once

#include "CoreMinimal.h"
#include "WebRTC4UnrealTypes.h"

class UWebRTC4UnrealSubsystem;

using FWebRTC4UnrealProviderCompletion = TFunction<void(FWebRTC4UnrealOperationResult)>;

/**
 * Control-plane boundary. A provider resolves a logical room and, when it
 * needs a custom data plane, registers an opaque transport context through
 * FWebRTC4UnrealTransportRegistry before completing Host or Join.
 */
class WEBRTC4UNREALCORE_API IWebRTC4UnrealProvider
{
public:
	virtual ~IWebRTC4UnrealProvider() = default;

	virtual FName GetProviderName() const = 0;
	virtual void Configure(const FWebRTC4UnrealProviderConfiguration& InConfiguration) = 0;
	virtual void Host(const FWebRTC4UnrealHostRequest& Request, FWebRTC4UnrealProviderCompletion Completion) = 0;
	virtual void Join(const FWebRTC4UnrealJoinRequest& Request, FWebRTC4UnrealProviderCompletion Completion) = 0;
	virtual void Leave() = 0;
};

/** Runtime factory registry used by provider modules. */
class WEBRTC4UNREALCORE_API FWebRTC4UnrealProviderRegistry
{
public:
	using FFactory = TFunction<TSharedRef<IWebRTC4UnrealProvider>(UWebRTC4UnrealSubsystem&)>;

	static void RegisterFactory(FName ProviderName, FFactory Factory);
	static void UnregisterFactory(FName ProviderName);
	static TSharedPtr<IWebRTC4UnrealProvider> Create(FName ProviderName, UWebRTC4UnrealSubsystem& Owner);
	static TArray<FName> GetProviderNames();
};
