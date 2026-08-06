#pragma once

#include "CoreMinimal.h"
#include "IpConnection.h"
#include "IpNetDriver.h"
#include "IWebRTC4UnrealTransport.h"
#include "WebRTC4UnrealNetDriver.generated.h"

UCLASS(MinimalAPI, transient, config = Engine)
class UWebRTC4UnrealNetConnection : public UIpConnection
{
	GENERATED_UCLASS_BODY()

public:
	virtual void InitBase(UNetDriver* InDriver, FSocket* InSocket, const FURL& InURL,
		EConnectionState InState, int32 InMaxPacket = 0, int32 InPacketOverhead = 0) override;
	virtual void InitRemoteConnection(UNetDriver* InDriver, FSocket* InSocket, const FURL& InURL,
		const FInternetAddr& InRemoteAddr, EConnectionState InState, int32 InMaxPacket = 0,
		int32 InPacketOverhead = 0) override;
	virtual void InitLocalConnection(UNetDriver* InDriver, FSocket* InSocket, const FURL& InURL,
		EConnectionState InState, int32 InMaxPacket = 0, int32 InPacketOverhead = 0) override;
	virtual void LowLevelSend(void* Data, int32 CountBits, FOutPacketTraits& Traits) override;
	virtual FString LowLevelGetRemoteAddress(bool bAppendPort = false) override;
	virtual FString LowLevelDescribe() override;
	virtual void Tick(float DeltaSeconds) override;

	void SetTransportPeerId(FString InPeerId) { TransportPeerId = MoveTemp(InPeerId); }
	const FString& GetTransportPeerId() const { return TransportPeerId; }
	void ReceivedTransportPacket(TArray<uint8>&& Packet);

private:
	FString TransportPeerId;
};

/**
 * Provider-neutral NetDriver inspired by EOS passthrough behavior. Normal IP
 * URLs use UIpNetDriver unchanged. URLs carrying WebRTC4Unreal transport
 * options are routed through a registered datagram endpoint, with one
 * UNetConnection per remote peer.
 */
UCLASS(MinimalAPI, transient, config = Engine)
class UWebRTC4UnrealNetDriver : public UIpNetDriver
{
	GENERATED_UCLASS_BODY()

public:
	virtual bool InitConnect(FNetworkNotify* InNotify, const FURL& ConnectURL, FString& Error) override;
	virtual bool InitListen(FNetworkNotify* InNotify, FURL& LocalURL, bool bReuseAddressAndPort,
		FString& Error) override;
	virtual void TickDispatch(float DeltaTime) override;
	virtual void LowLevelSend(TSharedPtr<const FInternetAddr> Address, void* Data, int32 CountBits,
		FOutPacketTraits& Traits) override;
	virtual FString LowLevelGetNetworkNumber() override;
	virtual void LowLevelDestroy() override;
	virtual bool IsNetResourceValid() override;

	bool IsUsingCustomTransport() const { return bUsingCustomTransport; }
	bool SendTransportPacket(const FString& PeerId, const uint8* Data, int32 NumBytes);

private:
	bool FindTransportRegistration(const FURL& URL, FName& OutTransportName, FString& OutContextKey,
		FString& Error) const;
	bool StartTransport(FName InTransportName, const FString& InContextKey, bool bHost, FString& Error);
	void OnTransportPeerConnected(const FString& PeerId);
	void OnTransportPacket(const FString& PeerId, TArray<uint8>&& Packet);
	void OnTransportPeerDisconnected(const FString& PeerId);
	void OnTransportError(const FString& PeerId, const FString& Error);
	void OnTransportClosed();
	void FlushOutgoing(const FString& QueueKey, const FString& PeerId);
	void FlushIncoming(const FString& PeerId, UWebRTC4UnrealNetConnection& Connection);
	void CloseAllConnections();

	bool bUsingCustomTransport = false;
	bool bHostTransport = false;
	FName TransportName;
	FString TransportContextKey;
	FString ClientServerPeerId;
	TSharedPtr<IWebRTC4UnrealTransportEndpoint> TransportEndpoint;
	TMap<FString, TWeakObjectPtr<UWebRTC4UnrealNetConnection>> PeerConnections;
	TMap<FString, TArray<TArray<uint8>>> PendingOutgoingPackets;
	TMap<FString, TArray<TArray<uint8>>> PendingIncomingPackets;
	int32 PendingPacketBytes = 0;
	int32 NextSyntheticPort = 20000;
};
