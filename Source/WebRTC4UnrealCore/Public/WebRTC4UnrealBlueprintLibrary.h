#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "WebRTC4UnrealBlueprintLibrary.generated.h"

class UWebRTC4UnrealSubsystem;

UCLASS()
class WEBRTC4UNREALCORE_API UWebRTC4UnrealBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "WebRTC4Unreal", meta = (WorldContext = "WorldContextObject"))
	static UWebRTC4UnrealSubsystem* GetWebRTC4UnrealSubsystem(const UObject* WorldContextObject);
};
