#include "FlowWebRTCBlueprintLibrary.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "WebRTC4UnrealSubsystem.h"

namespace
{
	UWebRTC4UnrealSubsystem* ResolveSubsystem(const UObject* WorldContextObject)
	{
		const UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UWebRTC4UnrealSubsystem>() : nullptr;
	}

	void ConfigureFlow(UWebRTC4UnrealSubsystem& Subsystem, const FString& Endpoint,
		const FString& BootstrapKey)
	{
		FWebRTC4UnrealProviderConfiguration Configuration;
		Configuration.Endpoint = Endpoint;
		Configuration.Options.Add(TEXT("SessionBroker"), TEXT("true"));
		Configuration.Options.Add(TEXT("AccessKey"), BootstrapKey);
		Subsystem.ConfigureProvider(TEXT("Flow"), Configuration);
	}
}

void UFlowWebRTCBlueprintLibrary::HostFlowSessionWithBackend(
	const UObject* WorldContextObject, const FString& BackendEndpoint,
	const FString& BootstrapKey, const FWebRTC4UnrealHostRequest& Request)
{
	if (UWebRTC4UnrealSubsystem* Subsystem = ResolveSubsystem(WorldContextObject))
	{
		ConfigureFlow(*Subsystem, BackendEndpoint, BootstrapKey);
		Subsystem->HostSession(TEXT("Flow"), Request);
	}
}

void UFlowWebRTCBlueprintLibrary::JoinFlowSessionWithBackend(
	const UObject* WorldContextObject, const FString& BackendEndpoint,
	const FString& BootstrapKey, const FWebRTC4UnrealJoinRequest& Request)
{
	if (UWebRTC4UnrealSubsystem* Subsystem = ResolveSubsystem(WorldContextObject))
	{
		ConfigureFlow(*Subsystem, BackendEndpoint, BootstrapKey);
		Subsystem->JoinSession(TEXT("Flow"), Request);
	}
}
