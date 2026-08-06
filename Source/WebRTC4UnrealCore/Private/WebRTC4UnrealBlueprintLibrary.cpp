#include "WebRTC4UnrealBlueprintLibrary.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "WebRTC4UnrealSubsystem.h"

UWebRTC4UnrealSubsystem* UWebRTC4UnrealBlueprintLibrary::GetWebRTC4UnrealSubsystem(const UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}

	const UWorld* World = WorldContextObject->GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UWebRTC4UnrealSubsystem>() : nullptr;
}
