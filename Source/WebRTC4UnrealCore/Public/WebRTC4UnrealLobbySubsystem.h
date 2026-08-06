#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDeviceFile.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "WebRTC4UnrealLobbySubsystem.generated.h"

class UWebRTC4UnrealLobbyWidget;
class UWebRTC4UnrealNetworkStatusWidget;

/** Creates and owns the default lobby widget for game clients. */
UCLASS(BlueprintType)
class WEBRTC4UNREALCORE_API UWebRTC4UnrealLobbySubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return (bEnabled || bNetworkStatusEnabled) && !IsTemplate() && !HasAnyFlags(RF_ClassDefaultObject); }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UWebRTC4UnrealLobbySubsystem, STATGROUP_Tickables); }
	virtual UWorld* GetTickableGameObjectWorld() const override;

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal|UI")
	void ShowLobby();

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal|UI")
	void HideLobby();

	UFUNCTION(BlueprintPure, Category = "WebRTC4Unreal|UI")
	UWebRTC4UnrealLobbyWidget* GetLobbyWidget() const { return LobbyWidget.Get(); }

private:
	void TryCreateLobby();
	void TryCreateNetworkStatus();
	bool HasConnectedRemoteClient() const;
	FString DetectAdvertisedAddress() const;
	void ApplyLobbyInputMode(bool bLobbyVisible) const;
	void StartDebugTextLog();

	bool bEnabled = false;
	bool bNetworkStatusEnabled = false;
	bool bShouldShow = true;
	bool bHostAutoHideLogged = false;
	bool bWriteDebugTextLog = false;
	FName DefaultProvider;
	FString ProviderEndpoint;
	FString ProviderAccessKey;
	FString AdvertisedAddress;
	int32 ListenPort = 7777;
	int32 MaxParticipants = 4;
	TUniquePtr<FOutputDeviceFile> DebugTextLog;

	UPROPERTY(Transient)
	TWeakObjectPtr<UWebRTC4UnrealLobbyWidget> LobbyWidget;

	UPROPERTY(Transient)
	TWeakObjectPtr<UWebRTC4UnrealNetworkStatusWidget> NetworkStatusWidget;
};
