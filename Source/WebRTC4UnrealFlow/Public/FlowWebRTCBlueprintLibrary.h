#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "WebRTC4UnrealTypes.h"
#include "FlowWebRTCBlueprintLibrary.generated.h"

UCLASS()
class WEBRTC4UNREALFLOW_API UFlowWebRTCBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal|Flow",
		meta = (WorldContext = "WorldContextObject"))
	static void HostFlowSessionWithBackend(const UObject* WorldContextObject,
		const FString& BackendEndpoint, const FString& BootstrapKey,
		const FWebRTC4UnrealHostRequest& Request);

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal|Flow",
		meta = (WorldContext = "WorldContextObject"))
	static void JoinFlowSessionWithBackend(const UObject* WorldContextObject,
		const FString& BackendEndpoint, const FString& BootstrapKey,
		const FWebRTC4UnrealJoinRequest& Request);
};
