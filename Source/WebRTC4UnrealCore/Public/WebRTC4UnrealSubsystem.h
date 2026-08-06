#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "WebRTC4UnrealTypes.h"
#include "WebRTC4UnrealSubsystem.generated.h"

class IWebRTC4UnrealProvider;
class APlayerController;
class AWebRTC4UnrealReplicationProbe;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FWebRTC4UnrealStateChangedSignature,
	EWebRTC4UnrealConnectionState, NewState, const FString&, Detail);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FWebRTC4UnrealOperationCompletedSignature,
	const FWebRTC4UnrealOperationResult&, Result);

/** Blueprint entry point and lifetime owner for provider modules. */
UCLASS(BlueprintType)
class WEBRTC4UNREALCORE_API UWebRTC4UnrealSubsystem : public UGameInstanceSubsystem,
	public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return !IsTemplate() && !HasAnyFlags(RF_ClassDefaultObject); }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UWebRTC4UnrealSubsystem, STATGROUP_Tickables);
	}
	virtual UWorld* GetTickableGameObjectWorld() const override;

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal")
	void ConfigureProvider(FName ProviderName,
		const FWebRTC4UnrealProviderConfiguration& Configuration);

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal")
	void HostSession(FName ProviderName, const FWebRTC4UnrealHostRequest& Request);

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal")
	void JoinSession(FName ProviderName, const FWebRTC4UnrealJoinRequest& Request);

	UFUNCTION(BlueprintCallable, Category = "WebRTC4Unreal")
	void LeaveSession();

	UFUNCTION(BlueprintPure, Category = "WebRTC4Unreal")
	EWebRTC4UnrealConnectionState GetConnectionState() const { return State; }

	UFUNCTION(BlueprintPure, Category = "WebRTC4Unreal")
	FWebRTC4UnrealSessionDescriptor GetCurrentSession() const { return CurrentSession; }

	UFUNCTION(BlueprintPure, Category = "WebRTC4Unreal|Network")
	FWebRTC4UnrealNetworkStatus GetNetworkStatus() const;

	UFUNCTION(BlueprintPure, Category = "WebRTC4Unreal")
	TArray<FName> GetAvailableProviders() const;

	UPROPERTY(BlueprintAssignable, Category = "WebRTC4Unreal")
	FWebRTC4UnrealStateChangedSignature OnStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "WebRTC4Unreal")
	FWebRTC4UnrealOperationCompletedSignature OnOperationCompleted;

private:
	TSharedPtr<IWebRTC4UnrealProvider> GetOrCreateProvider(FName ProviderName);
	void SetState(EWebRTC4UnrealConnectionState NewState, const FString& Detail);
	void CompleteHost(FName ProviderName, FWebRTC4UnrealHostRequest Request,
		FWebRTC4UnrealOperationResult Result);
	void CompleteJoin(FName ProviderName, FWebRTC4UnrealOperationResult Result);
	void TickAutomation(float DeltaTime);
	void StartAutomationIfReady();
	void TickConnectionDetection();
	void EnsureAutomationReplicationProbes();
	bool AreAutomationReplicationProbesReady() const;
	bool IsClientReplicationProbeReady() const;

	TMap<FName, FWebRTC4UnrealProviderConfiguration> Configurations;
	TMap<FName, TSharedPtr<IWebRTC4UnrealProvider>> Providers;

	UPROPERTY(Transient)
	EWebRTC4UnrealConnectionState State = EWebRTC4UnrealConnectionState::Idle;

	UPROPERTY(Transient)
	FWebRTC4UnrealSessionDescriptor CurrentSession;

	FName ActiveProvider;
	FString AutomationRole;
	FString AutomationProvider;
	FString AutomationRoom;
	FString AutomationConnect;
	FString AutomationAdvertise;
	FString AutomationMap;
	FString AutomationProviderEndpoint;
	FString AutomationAccessKey;
	int32 AutomationPort = 7777;
	int32 AutomationExpectedPlayers = 2;
	int32 NextAutomationProbeId = 1;
	bool bAutomationRequested = false;
	bool bAutomationVerifyRPC = false;
	bool bAutomationStarted = false;
	bool bHostReadyLogged = false;
	bool bPassLogged = false;
	bool bAutomationExitRequested = false;
	double AutomationStartSeconds = 0.0;
	double PassSeconds = 0.0;
	TMap<TWeakObjectPtr<APlayerController>, TWeakObjectPtr<AWebRTC4UnrealReplicationProbe>>
		AutomationReplicationProbes;
};
