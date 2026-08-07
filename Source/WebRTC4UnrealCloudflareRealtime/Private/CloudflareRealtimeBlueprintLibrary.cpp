#include "CloudflareRealtimeBlueprintLibrary.h"

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

	void ConfigureCloudflare(UWebRTC4UnrealSubsystem& Subsystem,
		const FName Provider, const FString& Endpoint, const FString& ClientAccessKey)
	{
		FWebRTC4UnrealProviderConfiguration Configuration;
		Configuration.Endpoint = Endpoint;
		Configuration.Options.Add(TEXT("AccessKey"), ClientAccessKey);
		Subsystem.ConfigureProvider(Provider, Configuration);
	}

	void ConfigureHybrid(UWebRTC4UnrealSubsystem& Subsystem,
		const FString& Endpoint, const FString& ClientAccessKey,
		float FallbackTimeoutSeconds, bool bForceCloudflareFallback)
	{
		FWebRTC4UnrealProviderConfiguration Configuration;
		Configuration.Endpoint = Endpoint;
		Configuration.Options.Add(TEXT("AccessKey"), ClientAccessKey);
		Configuration.Options.Add(TEXT("SessionBroker"), TEXT("true"));
		Configuration.Options.Add(TEXT("FallbackTimeoutSeconds"),
			FString::SanitizeFloat(FMath::Clamp(FallbackTimeoutSeconds, 2.0f, 30.0f)));
		Configuration.Options.Add(TEXT("ForceCloudflareFallback"),
			bForceCloudflareFallback ? TEXT("true") : TEXT("false"));
		Subsystem.ConfigureProvider(TEXT("FlowCloudflareFallback"), Configuration);
	}
}

void UCloudflareRealtimeBlueprintLibrary::HostCloudflareRealtimeSession(
	const UObject* WorldContextObject, const FString& WorkerEndpoint,
	const FString& ClientAccessKey, const FWebRTC4UnrealHostRequest& Request)
{
	if (UWebRTC4UnrealSubsystem* Subsystem = ResolveSubsystem(WorldContextObject))
	{
		ConfigureCloudflare(*Subsystem, TEXT("CloudflareRealtime"), WorkerEndpoint, ClientAccessKey);
		Subsystem->HostSession(TEXT("CloudflareRealtime"), Request);
	}
}

void UCloudflareRealtimeBlueprintLibrary::JoinCloudflareRealtimeSession(
	const UObject* WorldContextObject, const FString& WorkerEndpoint,
	const FString& ClientAccessKey, const FWebRTC4UnrealJoinRequest& Request)
{
	if (UWebRTC4UnrealSubsystem* Subsystem = ResolveSubsystem(WorldContextObject))
	{
		ConfigureCloudflare(*Subsystem, TEXT("CloudflareRealtime"), WorkerEndpoint, ClientAccessKey);
		Subsystem->JoinSession(TEXT("CloudflareRealtime"), Request);
	}
}

void UCloudflareRealtimeBlueprintLibrary::HostCloudflareDirectSession(
	const UObject* WorldContextObject, const FString& WorkerEndpoint,
	const FString& ClientAccessKey, const FWebRTC4UnrealHostRequest& Request)
{
	if (UWebRTC4UnrealSubsystem* Subsystem = ResolveSubsystem(WorldContextObject))
	{
		ConfigureCloudflare(*Subsystem, TEXT("CloudflareDirect"), WorkerEndpoint, ClientAccessKey);
		Subsystem->HostSession(TEXT("CloudflareDirect"), Request);
	}
}

void UCloudflareRealtimeBlueprintLibrary::JoinCloudflareDirectSession(
	const UObject* WorldContextObject, const FString& WorkerEndpoint,
	const FString& ClientAccessKey, const FWebRTC4UnrealJoinRequest& Request)
{
	if (UWebRTC4UnrealSubsystem* Subsystem = ResolveSubsystem(WorldContextObject))
	{
		ConfigureCloudflare(*Subsystem, TEXT("CloudflareDirect"), WorkerEndpoint, ClientAccessKey);
		Subsystem->JoinSession(TEXT("CloudflareDirect"), Request);
	}
}

void UCloudflareRealtimeBlueprintLibrary::HostFlowWithCloudflareFallbackSession(
	const UObject* WorldContextObject, const FString& WorkerEndpoint,
	const FString& ClientAccessKey, const FWebRTC4UnrealHostRequest& Request,
	float FallbackTimeoutSeconds, bool bForceCloudflareFallback)
{
	if (UWebRTC4UnrealSubsystem* Subsystem = ResolveSubsystem(WorldContextObject))
	{
		ConfigureHybrid(*Subsystem, WorkerEndpoint, ClientAccessKey,
			FallbackTimeoutSeconds, bForceCloudflareFallback);
		Subsystem->HostSession(TEXT("FlowCloudflareFallback"), Request);
	}
}

void UCloudflareRealtimeBlueprintLibrary::JoinFlowWithCloudflareFallbackSession(
	const UObject* WorldContextObject, const FString& WorkerEndpoint,
	const FString& ClientAccessKey, const FWebRTC4UnrealJoinRequest& Request,
	float FallbackTimeoutSeconds, bool bForceCloudflareFallback)
{
	if (UWebRTC4UnrealSubsystem* Subsystem = ResolveSubsystem(WorldContextObject))
	{
		ConfigureHybrid(*Subsystem, WorkerEndpoint, ClientAccessKey,
			FallbackTimeoutSeconds, bForceCloudflareFallback);
		Subsystem->JoinSession(TEXT("FlowCloudflareFallback"), Request);
	}
}
