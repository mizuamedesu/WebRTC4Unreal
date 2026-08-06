#include "WebRTC4UnrealReplicationProbe.h"

#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(WebRTC4UnrealReplicationProbe)

DEFINE_LOG_CATEGORY_STATIC(LogWebRTC4UnrealReplicationProbe, Log, All);

AWebRTC4UnrealReplicationProbe::AWebRTC4UnrealReplicationProbe()
{
	bReplicates = true;
	bOnlyRelevantToOwner = true;
	bNetLoadOnClient = false;
	SetNetUpdateFrequency(30.0f);
	PrimaryActorTick.bCanEverTick = true;
}

void AWebRTC4UnrealReplicationProbe::InitializeProbe(int32 InProbeId)
{
	check(HasAuthority());
	ProbeId = InProbeId;
	ForceNetUpdate();
}

void AWebRTC4UnrealReplicationProbe::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	TrySendClientAcknowledgement();
}

void AWebRTC4UnrealReplicationProbe::OnRep_ProbeId()
{
	TrySendClientAcknowledgement();
}

void AWebRTC4UnrealReplicationProbe::TrySendClientAcknowledgement()
{
	if (bSentClientAcknowledgement || ProbeId <= 0 || GetNetMode() != NM_Client)
	{
		return;
	}

	const APlayerController* OwnerController = Cast<APlayerController>(GetOwner());
	if (!OwnerController || !OwnerController->IsLocalController())
	{
		return;
	}

	bSentClientAcknowledgement = true;
	UE_LOG(LogWebRTC4UnrealReplicationProbe, Display,
		TEXT("WEBRTC4UNREAL_RPC_REPLICATION_PASS probe=%d"), ProbeId);
	ServerAcknowledge(ProbeId);
}

void AWebRTC4UnrealReplicationProbe::ServerAcknowledge_Implementation(int32 InProbeId)
{
	if (InProbeId != ProbeId || ProbeId <= 0)
	{
		UE_LOG(LogWebRTC4UnrealReplicationProbe, Error,
			TEXT("WEBRTC4UNREAL_RPC_SERVER_REJECT expected=%d received=%d"),
			ProbeId, InProbeId);
		return;
	}

	bServerAcknowledged = true;
	ServerAck = ProbeId;
	ForceNetUpdate();
	UE_LOG(LogWebRTC4UnrealReplicationProbe, Display,
		TEXT("WEBRTC4UNREAL_RPC_SERVER_PASS probe=%d"), ProbeId);
	ClientConfirm(ProbeId);
	MulticastConfirm(ProbeId);
}

void AWebRTC4UnrealReplicationProbe::ClientConfirm_Implementation(int32 InProbeId)
{
	if (InProbeId == ProbeId && ProbeId > 0)
	{
		bClientRpcReceived = true;
		UE_LOG(LogWebRTC4UnrealReplicationProbe, Display,
			TEXT("WEBRTC4UNREAL_RPC_CLIENT_PASS probe=%d"), ProbeId);
	}
}

void AWebRTC4UnrealReplicationProbe::MulticastConfirm_Implementation(int32 InProbeId)
{
	if (GetNetMode() == NM_Client && InProbeId == ProbeId && ProbeId > 0)
	{
		bMulticastReceived = true;
		UE_LOG(LogWebRTC4UnrealReplicationProbe, Display,
			TEXT("WEBRTC4UNREAL_RPC_MULTICAST_PASS probe=%d"), ProbeId);
	}
}

void AWebRTC4UnrealReplicationProbe::OnRep_ServerAck()
{
	if (ServerAck == ProbeId && ProbeId > 0)
	{
		UE_LOG(LogWebRTC4UnrealReplicationProbe, Display,
			TEXT("WEBRTC4UNREAL_RPC_PROPERTY_PASS probe=%d"), ProbeId);
	}
}

bool AWebRTC4UnrealReplicationProbe::HasCompletedClientRoundTrip() const
{
	return ProbeId > 0 && ServerAck == ProbeId && bClientRpcReceived
		&& bMulticastReceived;
}

void AWebRTC4UnrealReplicationProbe::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWebRTC4UnrealReplicationProbe, ProbeId);
	DOREPLIFETIME(AWebRTC4UnrealReplicationProbe, ServerAck);
}
