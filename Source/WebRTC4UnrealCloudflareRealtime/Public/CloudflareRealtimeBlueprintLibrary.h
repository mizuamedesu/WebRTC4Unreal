#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "WebRTC4UnrealTypes.h"
#include "CloudflareRealtimeBlueprintLibrary.generated.h"

UCLASS()
class WEBRTC4UNREALCLOUDFLAREREALTIME_API UCloudflareRealtimeBlueprintLibrary
	: public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal|Cloudflare Realtime",
		meta = (WorldContext = "WorldContextObject"))
	static void HostCloudflareRealtimeSession(const UObject* WorldContextObject,
		const FString& WorkerEndpoint, const FString& ClientAccessKey,
		const FWebRTC4UnrealHostRequest& Request);

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal|Cloudflare Realtime",
		meta = (WorldContext = "WorldContextObject"))
	static void JoinCloudflareRealtimeSession(const UObject* WorldContextObject,
		const FString& WorkerEndpoint, const FString& ClientAccessKey,
		const FWebRTC4UnrealJoinRequest& Request);

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal|Cloudflare Direct",
		meta = (WorldContext = "WorldContextObject"))
	static void HostCloudflareDirectSession(const UObject* WorldContextObject,
		const FString& WorkerEndpoint, const FString& ClientAccessKey,
		const FWebRTC4UnrealHostRequest& Request);

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal|Cloudflare Direct",
		meta = (WorldContext = "WorldContextObject"))
	static void JoinCloudflareDirectSession(const UObject* WorldContextObject,
		const FString& WorkerEndpoint, const FString& ClientAccessKey,
		const FWebRTC4UnrealJoinRequest& Request);

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal|Flow + Cloudflare",
		meta = (WorldContext = "WorldContextObject", AdvancedDisplay = "FallbackTimeoutSeconds,bForceCloudflareFallback"))
	static void HostFlowWithCloudflareFallbackSession(const UObject* WorldContextObject,
		const FString& WorkerEndpoint, const FString& ClientAccessKey,
		const FWebRTC4UnrealHostRequest& Request, float FallbackTimeoutSeconds = 8.0f,
		bool bForceCloudflareFallback = false);

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal|Flow + Cloudflare",
		meta = (WorldContext = "WorldContextObject", AdvancedDisplay = "FallbackTimeoutSeconds,bForceCloudflareFallback"))
	static void JoinFlowWithCloudflareFallbackSession(const UObject* WorldContextObject,
		const FString& WorkerEndpoint, const FString& ClientAccessKey,
		const FWebRTC4UnrealJoinRequest& Request, float FallbackTimeoutSeconds = 8.0f,
		bool bForceCloudflareFallback = false);
};
