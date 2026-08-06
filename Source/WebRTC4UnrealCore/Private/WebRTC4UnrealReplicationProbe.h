#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WebRTC4UnrealReplicationProbe.generated.h"

/** Runtime automation probe that exercises Unreal's normal replication/RPC path. */
UCLASS(NotBlueprintable, Transient)
class AWebRTC4UnrealReplicationProbe final : public AActor
{
	GENERATED_BODY()

public:
	AWebRTC4UnrealReplicationProbe();
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void InitializeProbe(int32 InProbeId);
	bool HasServerAcknowledgement() const { return bServerAcknowledged; }
	bool HasCompletedClientRoundTrip() const;
	int32 GetProbeId() const { return ProbeId; }

private:
	UFUNCTION()
	void OnRep_ProbeId();

	UFUNCTION()
	void OnRep_ServerAck();

	UFUNCTION(Server, Reliable)
	void ServerAcknowledge(int32 InProbeId);

	UFUNCTION(Client, Reliable)
	void ClientConfirm(int32 InProbeId);

	UFUNCTION(NetMulticast, Reliable)
	void MulticastConfirm(int32 InProbeId);

	void TrySendClientAcknowledgement();

	UPROPERTY(ReplicatedUsing = OnRep_ProbeId)
	int32 ProbeId = 0;

	UPROPERTY(ReplicatedUsing = OnRep_ServerAck)
	int32 ServerAck = 0;

	bool bSentClientAcknowledgement = false;
	bool bServerAcknowledged = false;
	bool bClientRpcReceived = false;
	bool bMulticastReceived = false;
};
