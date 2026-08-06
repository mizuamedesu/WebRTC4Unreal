#include "WebRTC4UnrealNetDriver.h"

#include "Async/Async.h"
#include "Net/DataChannel.h"
#include "PacketHandler.h"
#include "SocketSubsystem.h"
#include "WebRTC4UnrealTransportRegistry.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(WebRTC4UnrealNetDriver)

DEFINE_LOG_CATEGORY_STATIC(LogWebRTC4UnrealNetDriver, Log, All);

namespace
{
	constexpr int32 TransportMaxPacket = MAX_PACKET_SIZE;
	constexpr int32 MaxPendingTransportBytes = 8 * 1024 * 1024;
	const FString ClientPendingQueueKey(TEXT("__webRTC4Unreal_server__"));
}

UWebRTC4UnrealNetConnection::UWebRTC4UnrealNetConnection(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UWebRTC4UnrealNetConnection::InitBase(UNetDriver* InDriver, FSocket* InSocket, const FURL& InURL,
	EConnectionState InState, int32 InMaxPacket, int32 InPacketOverhead)
{
	if (const UWebRTC4UnrealNetDriver* TransportDriver = Cast<UWebRTC4UnrealNetDriver>(InDriver);
		TransportDriver && TransportDriver->IsUsingCustomTransport())
	{
		UNetConnection::InitBase(InDriver, nullptr, InURL, InState,
			InMaxPacket == 0 ? TransportMaxPacket : InMaxPacket,
			InPacketOverhead == 0 ? 1 : InPacketOverhead);
		// The provider has already authenticated and associated a fixed peer.
		// Connectionless IP cookies would alter packets that never use UDP.
		Handler.Reset();
		return;
	}
	Super::InitBase(InDriver, InSocket, InURL, InState, InMaxPacket, InPacketOverhead);
}

void UWebRTC4UnrealNetConnection::InitRemoteConnection(UNetDriver* InDriver, FSocket* InSocket,
	const FURL& InURL, const FInternetAddr& InRemoteAddr, EConnectionState InState,
	int32 InMaxPacket, int32 InPacketOverhead)
{
	if (const UWebRTC4UnrealNetDriver* TransportDriver = Cast<UWebRTC4UnrealNetDriver>(InDriver);
		TransportDriver && TransportDriver->IsUsingCustomTransport())
	{
		InitBase(InDriver, nullptr, InURL, InState, InMaxPacket, InPacketOverhead);
		InitSendBuffer();
		SetClientLoginState(EClientLoginState::LoggingIn);
		SetExpectedClientLoginMsgType(NMT_Hello);
		return;
	}
	Super::InitRemoteConnection(InDriver, InSocket, InURL, InRemoteAddr, InState,
		InMaxPacket, InPacketOverhead);
}

void UWebRTC4UnrealNetConnection::InitLocalConnection(UNetDriver* InDriver, FSocket* InSocket,
	const FURL& InURL, EConnectionState InState, int32 InMaxPacket, int32 InPacketOverhead)
{
	if (const UWebRTC4UnrealNetDriver* TransportDriver = Cast<UWebRTC4UnrealNetDriver>(InDriver);
		TransportDriver && TransportDriver->IsUsingCustomTransport())
	{
		InitBase(InDriver, nullptr, InURL, InState, InMaxPacket, InPacketOverhead);
		InitSendBuffer();
		return;
	}
	Super::InitLocalConnection(InDriver, InSocket, InURL, InState, InMaxPacket, InPacketOverhead);
}

void UWebRTC4UnrealNetConnection::LowLevelSend(void* Data, int32 CountBits, FOutPacketTraits& Traits)
{
	UWebRTC4UnrealNetDriver* TransportDriver = Cast<UWebRTC4UnrealNetDriver>(Driver);
	if (!TransportDriver || !TransportDriver->IsUsingCustomTransport())
	{
		Super::LowLevelSend(Data, CountBits, Traits);
		return;
	}

	const uint8* DataToSend = static_cast<const uint8*>(Data);
	if (Handler.IsValid() && !Handler->GetRawSend())
	{
		const ProcessedPacket ProcessedData = Handler->Outgoing(static_cast<uint8*>(Data), CountBits, Traits);
		if (ProcessedData.bError)
		{
			return;
		}
		DataToSend = ProcessedData.Data;
		CountBits = ProcessedData.CountBits;
	}
	const int32 CountBytes = FMath::DivideAndRoundUp(CountBits, 8);
	if (CountBytes > 0)
	{
		TransportDriver->SendTransportPacket(TransportPeerId, DataToSend, CountBytes);
	}
}

FString UWebRTC4UnrealNetConnection::LowLevelGetRemoteAddress(bool bAppendPort)
{
	if (const UWebRTC4UnrealNetDriver* TransportDriver = Cast<UWebRTC4UnrealNetDriver>(Driver);
		TransportDriver && TransportDriver->IsUsingCustomTransport())
	{
		return bAppendPort
			? FString::Printf(TEXT("webRTC4Unreal:%s:0"), *TransportPeerId)
			: FString::Printf(TEXT("webRTC4Unreal:%s"), *TransportPeerId);
	}
	return Super::LowLevelGetRemoteAddress(bAppendPort);
}

FString UWebRTC4UnrealNetConnection::LowLevelDescribe()
{
	if (const UWebRTC4UnrealNetDriver* TransportDriver = Cast<UWebRTC4UnrealNetDriver>(Driver);
		TransportDriver && TransportDriver->IsUsingCustomTransport())
	{
		return FString::Printf(TEXT("transport-peer=%s state=%s"), *TransportPeerId,
			LexToString(GetConnectionState()));
	}
	return Super::LowLevelDescribe();
}

void UWebRTC4UnrealNetConnection::Tick(float DeltaSeconds)
{
	if (const UWebRTC4UnrealNetDriver* TransportDriver = Cast<UWebRTC4UnrealNetDriver>(Driver);
		TransportDriver && TransportDriver->IsUsingCustomTransport())
	{
		UNetConnection::Tick(DeltaSeconds);
		return;
	}
	Super::Tick(DeltaSeconds);
}

void UWebRTC4UnrealNetConnection::ReceivedTransportPacket(TArray<uint8>&& Packet)
{
	if (!Packet.IsEmpty() && Driver)
	{
		UNetConnection::ReceivedRawPacket(Packet.GetData(), Packet.Num());
	}
}

UWebRTC4UnrealNetDriver::UWebRTC4UnrealNetDriver(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool UWebRTC4UnrealNetDriver::FindTransportRegistration(const FURL& URL, FName& OutTransportName,
	FString& OutContextKey, FString& Error) const
{
	const TCHAR* TransportOption = URL.GetOption(TEXT("WebRTC4UnrealTransport="), nullptr);
	const TCHAR* ContextOption = URL.GetOption(TEXT("WebRTC4UnrealContext="), nullptr);
	if (!TransportOption || !*TransportOption)
	{
		return false;
	}
	if (!ContextOption || !*ContextOption)
	{
		Error = TEXT("WebRTC4Unreal transport URL is missing its in-memory context key");
		return false;
	}

	OutTransportName = FName(TransportOption);
	OutContextKey = ContextOption;
	FWebRTC4UnrealTransportRegistration Registration;
	if (!FWebRTC4UnrealTransportRegistry::FindContext(OutContextKey, Registration))
	{
		Error = FString::Printf(TEXT("No WebRTC4Unreal transport context exists for key %s"),
			*OutContextKey);
		return false;
	}
	if (Registration.TransportName != OutTransportName)
	{
		Error = TEXT("WebRTC4Unreal transport URL does not match its registered context");
		return false;
	}
	return true;
}

bool UWebRTC4UnrealNetDriver::InitConnect(FNetworkNotify* InNotify, const FURL& ConnectURL,
	FString& Error)
{
	if (!ConnectURL.GetOption(TEXT("WebRTC4UnrealTransport="), nullptr))
	{
		return Super::InitConnect(InNotify, ConnectURL, Error);
	}

	FName RequestedTransport;
	FString RequestedContext;
	if (!FindTransportRegistration(ConnectURL, RequestedTransport, RequestedContext, Error))
	{
		return false;
	}

	bUsingCustomTransport = true;
	bHostTransport = false;
	if (!UNetDriver::InitBase(true, InNotify, ConnectURL, false, Error))
	{
		return false;
	}
	ServerConnection = NewObject<UWebRTC4UnrealNetConnection>(NetConnectionClass);
	ServerConnection->InitLocalConnection(this, nullptr, ConnectURL, USOCK_Pending);
	CreateInitialClientChannels();
	return StartTransport(RequestedTransport, RequestedContext, false, Error);
}

bool UWebRTC4UnrealNetDriver::InitListen(FNetworkNotify* InNotify, FURL& LocalURL,
	bool bReuseAddressAndPort, FString& Error)
{
	if (!LocalURL.GetOption(TEXT("WebRTC4UnrealTransport="), nullptr))
	{
		return Super::InitListen(InNotify, LocalURL, bReuseAddressAndPort, Error);
	}

	FName RequestedTransport;
	FString RequestedContext;
	if (!FindTransportRegistration(LocalURL, RequestedTransport, RequestedContext, Error))
	{
		return false;
	}

	bUsingCustomTransport = true;
	bHostTransport = true;
	if (!UNetDriver::InitBase(false, InNotify, LocalURL, bReuseAddressAndPort, Error))
	{
		return false;
	}
	ServerConnection = nullptr;
	return StartTransport(RequestedTransport, RequestedContext, true, Error);
}

bool UWebRTC4UnrealNetDriver::StartTransport(FName InTransportName, const FString& InContextKey,
	bool bHost, FString& Error)
{
	FWebRTC4UnrealTransportRegistration Registration;
	if (!FWebRTC4UnrealTransportRegistry::FindContext(InContextKey, Registration)
		|| !Registration.Context || Registration.TransportName != InTransportName
		|| Registration.bHost != bHost)
	{
		Error = TEXT("WebRTC4Unreal transport context disappeared or has the wrong role");
		return false;
	}

	TransportEndpoint = FWebRTC4UnrealTransportRegistry::CreateEndpoint(InTransportName);
	if (!TransportEndpoint)
	{
		Error = FString::Printf(TEXT("Transport '%s' is not registered"), *InTransportName.ToString());
		return false;
	}

	TransportName = InTransportName;
	TransportContextKey = InContextKey;
	TWeakObjectPtr<UWebRTC4UnrealNetDriver> WeakThis(this);
	FWebRTC4UnrealTransportCallbacks Callbacks;
	Callbacks.OnPeerConnected = [WeakThis](const FString& PeerId)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, PeerId]()
		{
			if (WeakThis.IsValid()) WeakThis->OnTransportPeerConnected(PeerId);
		});
	};
	Callbacks.OnPacket = [WeakThis](const FString& PeerId, TArray<uint8>&& Packet)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, PeerId, Packet = MoveTemp(Packet)]() mutable
		{
			if (WeakThis.IsValid()) WeakThis->OnTransportPacket(PeerId, MoveTemp(Packet));
		});
	};
	Callbacks.OnPeerDisconnected = [WeakThis](const FString& PeerId)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, PeerId]()
		{
			if (WeakThis.IsValid()) WeakThis->OnTransportPeerDisconnected(PeerId);
		});
	};
	Callbacks.OnError = [WeakThis](const FString& PeerId, const FString& TransportError)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, PeerId, TransportError]()
		{
			if (WeakThis.IsValid()) WeakThis->OnTransportError(PeerId, TransportError);
		});
	};
	Callbacks.OnClosed = [WeakThis]()
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis]()
		{
			if (WeakThis.IsValid()) WeakThis->OnTransportClosed();
		});
	};

	if (!TransportEndpoint->Start(Registration.Context.ToSharedRef(), bHost, MoveTemp(Callbacks), Error))
	{
		TransportEndpoint.Reset();
		return false;
	}
	UE_LOG(LogWebRTC4UnrealNetDriver, Display,
		TEXT("WEBRTC4UNREAL_NETDRIVER_STARTED role=%s transport=%s context=%s"),
		bHost ? TEXT("host") : TEXT("client"), *TransportName.ToString(), *TransportContextKey);
	return true;
}

void UWebRTC4UnrealNetDriver::OnTransportPeerConnected(const FString& PeerId)
{
	if (PeerId.IsEmpty() || PeerConnections.Contains(PeerId))
	{
		return;
	}

	if (!bHostTransport)
	{
		if (!ClientServerPeerId.IsEmpty() && ClientServerPeerId != PeerId)
		{
			OnTransportError(PeerId, TEXT("Client transport exposed more than one server peer"));
			return;
		}
		ClientServerPeerId = PeerId;
		if (UWebRTC4UnrealNetConnection* Connection = Cast<UWebRTC4UnrealNetConnection>(ServerConnection))
		{
			Connection->SetTransportPeerId(PeerId);
			Connection->SetConnectionState(USOCK_Open);
			PeerConnections.Add(PeerId, Connection);
			FlushOutgoing(ClientPendingQueueKey, PeerId);
			FlushIncoming(PeerId, *Connection);
		}
		return;
	}

	if (!Notify || Notify->NotifyAcceptingConnection() != EAcceptConnection::Accept)
	{
		TransportEndpoint->ClosePeer(PeerId);
		return;
	}

	UWebRTC4UnrealNetConnection* Connection = NewObject<UWebRTC4UnrealNetConnection>(NetConnectionClass);
	Connection->SetTransportPeerId(PeerId);
	TSharedRef<FInternetAddr> Address = GetSocketSubsystem()->CreateInternetAddr();
	bool bValidAddress = false;
	Address->SetIp(TEXT("127.0.0.1"), bValidAddress);
	Address->SetPort(NextSyntheticPort++);
	Connection->InitRemoteConnection(this, nullptr, FURL(), *Address, USOCK_Open);
	Notify->NotifyAcceptedConnection(Connection);
	AddClientConnection(Connection);
	PeerConnections.Add(PeerId, Connection);
	FlushIncoming(PeerId, *Connection);
	UE_LOG(LogWebRTC4UnrealNetDriver, Display,
		TEXT("WEBRTC4UNREAL_NETCONNECTION_ACCEPTED peer=%s clients=%d transport=%s"),
		*PeerId, ClientConnections.Num(), *TransportName.ToString());
}

void UWebRTC4UnrealNetDriver::OnTransportPacket(const FString& PeerId, TArray<uint8>&& Packet)
{
	if (TWeakObjectPtr<UWebRTC4UnrealNetConnection>* Found = PeerConnections.Find(PeerId))
	{
		if (UWebRTC4UnrealNetConnection* Connection = Found->Get())
		{
			Connection->ReceivedTransportPacket(MoveTemp(Packet));
			return;
		}
	}
	if (PendingPacketBytes + Packet.Num() > MaxPendingTransportBytes)
	{
		OnTransportError(PeerId, TEXT("Incoming pre-connection packet queue exceeded 8 MiB"));
		return;
	}
	PendingPacketBytes += Packet.Num();
	PendingIncomingPackets.FindOrAdd(PeerId).Add(MoveTemp(Packet));
}

void UWebRTC4UnrealNetDriver::OnTransportPeerDisconnected(const FString& PeerId)
{
	if (TWeakObjectPtr<UWebRTC4UnrealNetConnection>* Found = PeerConnections.Find(PeerId))
	{
		if (UWebRTC4UnrealNetConnection* Connection = Found->Get())
		{
			Connection->Close();
		}
	}
	PeerConnections.Remove(PeerId);
	PendingIncomingPackets.Remove(PeerId);
	PendingOutgoingPackets.Remove(PeerId);
	if (ClientServerPeerId == PeerId)
	{
		ClientServerPeerId.Reset();
	}
	UE_LOG(LogWebRTC4UnrealNetDriver, Display,
		TEXT("WEBRTC4UNREAL_PEER_DISCONNECTED peer=%s remaining=%d"), *PeerId, PeerConnections.Num());
}

void UWebRTC4UnrealNetDriver::OnTransportError(const FString& PeerId, const FString& Error)
{
	UE_LOG(LogWebRTC4UnrealNetDriver, Error,
		TEXT("WEBRTC4UNREAL_NETDRIVER_ERROR peer=%s error=%s"), *PeerId, *Error);
	if (!PeerId.IsEmpty())
	{
		OnTransportPeerDisconnected(PeerId);
		return;
	}
	CloseAllConnections();
}

void UWebRTC4UnrealNetDriver::OnTransportClosed()
{
	CloseAllConnections();
}

bool UWebRTC4UnrealNetDriver::SendTransportPacket(const FString& PeerId, const uint8* Data,
	int32 NumBytes)
{
	if (!Data || NumBytes <= 0 || !TransportEndpoint)
	{
		return false;
	}
	const FString& Destination = PeerId.IsEmpty() && !bHostTransport ? ClientServerPeerId : PeerId;
	if (!Destination.IsEmpty() && PeerConnections.Contains(Destination)
		&& TransportEndpoint->Send(Destination, Data, NumBytes))
	{
		return true;
	}

	const FString QueueKey = Destination.IsEmpty() && !bHostTransport ? ClientPendingQueueKey : Destination;
	if (QueueKey.IsEmpty() || PendingPacketBytes + NumBytes > MaxPendingTransportBytes)
	{
		OnTransportError(Destination, TEXT("Outgoing pre-connection packet queue exceeded 8 MiB"));
		return false;
	}
	TArray<uint8>& Packet = PendingOutgoingPackets.FindOrAdd(QueueKey).AddDefaulted_GetRef();
	Packet.Append(Data, NumBytes);
	PendingPacketBytes += NumBytes;
	return true;
}

void UWebRTC4UnrealNetDriver::FlushOutgoing(const FString& QueueKey, const FString& PeerId)
{
	TArray<TArray<uint8>> Packets;
	if (TArray<TArray<uint8>>* Found = PendingOutgoingPackets.Find(QueueKey))
	{
		Packets = MoveTemp(*Found);
		PendingOutgoingPackets.Remove(QueueKey);
	}
	for (const TArray<uint8>& Packet : Packets)
	{
		PendingPacketBytes -= Packet.Num();
		if (!TransportEndpoint || !TransportEndpoint->Send(PeerId, Packet.GetData(), Packet.Num()))
		{
			OnTransportError(PeerId, TEXT("Failed to flush an Unreal packet after peer connection"));
			return;
		}
	}
}

void UWebRTC4UnrealNetDriver::FlushIncoming(const FString& PeerId,
	UWebRTC4UnrealNetConnection& Connection)
{
	TArray<TArray<uint8>> Packets;
	if (TArray<TArray<uint8>>* Found = PendingIncomingPackets.Find(PeerId))
	{
		Packets = MoveTemp(*Found);
		PendingIncomingPackets.Remove(PeerId);
	}
	for (TArray<uint8>& Packet : Packets)
	{
		PendingPacketBytes -= Packet.Num();
		Connection.ReceivedTransportPacket(MoveTemp(Packet));
	}
}

void UWebRTC4UnrealNetDriver::CloseAllConnections()
{
	if (ServerConnection)
	{
		ServerConnection->Close();
	}
	for (UNetConnection* Connection : ClientConnections)
	{
		if (Connection)
		{
			Connection->Close();
		}
	}
	PeerConnections.Reset();
	ClientServerPeerId.Reset();
}

void UWebRTC4UnrealNetDriver::TickDispatch(float DeltaTime)
{
	if (bUsingCustomTransport)
	{
		UNetDriver::TickDispatch(DeltaTime);
		return;
	}
	Super::TickDispatch(DeltaTime);
}

void UWebRTC4UnrealNetDriver::LowLevelSend(TSharedPtr<const FInternetAddr> Address, void* Data,
	int32 CountBits, FOutPacketTraits& Traits)
{
	if (!bUsingCustomTransport)
	{
		Super::LowLevelSend(Address, Data, CountBits, Traits);
		return;
	}
	if (!bHostTransport)
	{
		SendTransportPacket(ClientServerPeerId, static_cast<const uint8*>(Data),
			FMath::DivideAndRoundUp(CountBits, 8));
	}
}

FString UWebRTC4UnrealNetDriver::LowLevelGetNetworkNumber()
{
	return bUsingCustomTransport
		? FString::Printf(TEXT("webRTC4Unreal:%s:%s"), *TransportName.ToString(), *TransportContextKey)
		: Super::LowLevelGetNetworkNumber();
}

void UWebRTC4UnrealNetDriver::LowLevelDestroy()
{
	if (bUsingCustomTransport)
	{
		if (TransportEndpoint)
		{
			TransportEndpoint->Close();
			TransportEndpoint.Reset();
		}
		FWebRTC4UnrealTransportRegistry::RemoveContext(TransportContextKey);
		PendingOutgoingPackets.Reset();
		PendingIncomingPackets.Reset();
		PendingPacketBytes = 0;
		PeerConnections.Reset();
		UNetDriver::LowLevelDestroy();
		return;
	}
	Super::LowLevelDestroy();
}

bool UWebRTC4UnrealNetDriver::IsNetResourceValid()
{
	return bUsingCustomTransport
		? TransportEndpoint.IsValid() && TransportEndpoint->IsValid()
		: Super::IsNetResourceValid();
}
