#include "WebRTC4UnrealSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformMisc.h"
#include "IWebRTC4UnrealProvider.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "WebRTC4UnrealTransportRegistry.h"
#include "WebRTC4UnrealReplicationProbe.h"

DEFINE_LOG_CATEGORY_STATIC(LogWebRTC4UnrealSubsystem, Log, All);

namespace
{
	FString NetModeToString(ENetMode NetMode)
	{
		switch (NetMode)
		{
		case NM_Standalone: return TEXT("Standalone");
		case NM_DedicatedServer: return TEXT("DedicatedServer");
		case NM_ListenServer: return TEXT("ListenServer");
		case NM_Client: return TEXT("Client");
		default: return TEXT("Unknown");
		}
	}
}

void UWebRTC4UnrealSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	AutomationStartSeconds = FPlatformTime::Seconds();

	const TCHAR* CommandLine = FCommandLine::Get();
	bAutomationRequested = FParse::Value(CommandLine, TEXT("P2PTestRole="), AutomationRole);
	if (!bAutomationRequested)
	{
		return;
	}
	if (!FParse::Value(CommandLine, TEXT("WebRTC4UnrealProvider="), AutomationProvider)
		&& !FParse::Value(CommandLine, TEXT("P2PProvider="), AutomationProvider))
	{
		AutomationProvider = TEXT("DirectIp");
	}
	FParse::Value(CommandLine, TEXT("P2PRoom="), AutomationRoom);
	FParse::Value(CommandLine, TEXT("P2PConnect="), AutomationConnect);
	FParse::Value(CommandLine, TEXT("P2PAdvertise="), AutomationAdvertise);
	if (!FParse::Value(CommandLine, TEXT("WebRTC4UnrealEndpoint="),
		AutomationProviderEndpoint))
	{
		FParse::Value(CommandLine, TEXT("P2PProviderEndpoint="),
			AutomationProviderEndpoint);
	}
	FParse::Value(CommandLine, TEXT("WebRTC4UnrealAccessKey="), AutomationAccessKey);
	if (AutomationProviderEndpoint.IsEmpty())
	{
		GConfig->GetString(TEXT("WebRTC4Unreal"), TEXT("ProviderEndpoint"),
			AutomationProviderEndpoint, GGameIni);
		GConfig->GetString(TEXT("WebRTC4Unreal"), TEXT("ProviderAccessKey"),
			AutomationAccessKey, GGameIni);
	}
	if (!FParse::Value(CommandLine, TEXT("P2PMap="), AutomationMap))
	{
		AutomationMap = TEXT("/Game/FirstPerson/Lvl_FirstPerson");
	}
	FParse::Value(CommandLine, TEXT("P2PPort="), AutomationPort);
	FParse::Value(CommandLine, TEXT("P2PExpectedPlayers="), AutomationExpectedPlayers);
	AutomationExpectedPlayers = FMath::Clamp(AutomationExpectedPlayers, 2, 1000);
	bAutomationExitRequested = FParse::Param(CommandLine, TEXT("P2PTestAutoExit"));
	bAutomationVerifyRPC = FParse::Param(CommandLine, TEXT("P2PTestVerifyRPC"))
		|| FParse::Param(CommandLine, TEXT("WebRTC4UnrealTestVerifyRPC"));
	UE_LOG(LogWebRTC4UnrealSubsystem, Display,
		TEXT("P2PTEST_BOOT role=%s provider=%s expected_players=%d verify_rpc=%d"),
		*AutomationRole, *AutomationProvider, AutomationExpectedPlayers,
		bAutomationVerifyRPC);
}

void UWebRTC4UnrealSubsystem::Deinitialize()
{
	for (TPair<FName, TSharedPtr<IWebRTC4UnrealProvider>>& Pair : Providers)
	{
		if (Pair.Value)
		{
			Pair.Value->Leave();
		}
	}
	Providers.Empty();
	Super::Deinitialize();
}

UWorld* UWebRTC4UnrealSubsystem::GetTickableGameObjectWorld() const
{
	return GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
}

FWebRTC4UnrealNetworkStatus UWebRTC4UnrealSubsystem::GetNetworkStatus() const
{
	FWebRTC4UnrealNetworkStatus Result;
	Result.State = State;
	Result.Provider = CurrentSession.Provider;
	Result.TransportName = CurrentSession.TransportName;
	Result.bRelayOnly = CurrentSession.bRelayOnly;

	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return Result;
	}

	const ENetMode NetMode = World->GetNetMode();
	Result.bIsHost = NetMode == NM_ListenServer;
	UNetDriver* NetDriver = World->GetNetDriver();
	UNetConnection* PingConnection = nullptr;
	if (NetDriver && NetMode == NM_Client)
	{
		if (NetDriver->ServerConnection
			&& NetDriver->ServerConnection->GetConnectionState() == USOCK_Open)
		{
			Result.ConnectedPeerCount = 1;
			PingConnection = NetDriver->ServerConnection;
		}
	}
	else if (NetDriver && NetMode == NM_ListenServer)
	{
		for (UNetConnection* Candidate : NetDriver->ClientConnections)
		{
			if (Candidate && Candidate->GetConnectionState() == USOCK_Open)
			{
				++Result.ConnectedPeerCount;
				if (!PingConnection) PingConnection = Candidate;
			}
		}
	}
	Result.bConnected = Result.ConnectedPeerCount > 0;
	if (!PingConnection)
	{
		return Result;
	}

	double PingSeconds = PingConnection->AvgLag;
	if (PingSeconds <= 0.0)
	{
		PingSeconds = PingConnection->RawPingInSeconds;
	}
	if (PingSeconds > 0.0)
	{
		Result.PingMilliseconds = FMath::Clamp(static_cast<float>(PingSeconds * 1000.0), 0.0f, 9999.0f);
	}
	else if (PingConnection->PlayerController && PingConnection->PlayerController->PlayerState)
	{
		const float PlayerPing = PingConnection->PlayerController->PlayerState->GetPingInMilliseconds();
		if (PlayerPing > 0.0f)
		{
			Result.PingMilliseconds = FMath::Clamp(PlayerPing, 0.0f, 9999.0f);
		}
	}
	return Result;
}

void UWebRTC4UnrealSubsystem::Tick(float DeltaTime)
{
	if (bAutomationRequested)
	{
		TickAutomation(DeltaTime);
	}
	TickConnectionDetection();
}

void UWebRTC4UnrealSubsystem::ConfigureProvider(FName ProviderName,
	const FWebRTC4UnrealProviderConfiguration& Configuration)
{
	Configurations.Add(ProviderName, Configuration);
	if (TSharedPtr<IWebRTC4UnrealProvider> Provider = GetOrCreateProvider(ProviderName))
	{
		Provider->Configure(Configuration);
	}
}

void UWebRTC4UnrealSubsystem::HostSession(FName ProviderName,
	const FWebRTC4UnrealHostRequest& Request)
{
	TSharedPtr<IWebRTC4UnrealProvider> Provider = GetOrCreateProvider(ProviderName);
	if (!Provider)
	{
		const FWebRTC4UnrealOperationResult Result = FWebRTC4UnrealOperationResult::Failure(
			TEXT("provider_not_found"),
			FString::Printf(TEXT("WebRTC4Unreal provider '%s' is not registered"),
				*ProviderName.ToString()));
		SetState(EWebRTC4UnrealConnectionState::Failed, Result.Message);
		OnOperationCompleted.Broadcast(Result);
		return;
	}

	ActiveProvider = ProviderName;
	SetState(EWebRTC4UnrealConnectionState::Resolving,
		FString::Printf(TEXT("Creating room through %s"), *ProviderName.ToString()));
	TWeakObjectPtr<UWebRTC4UnrealSubsystem> WeakThis(this);
	Provider->Host(Request, [WeakThis, ProviderName, Request](FWebRTC4UnrealOperationResult Result)
	{
		if (WeakThis.IsValid())
		{
			WeakThis->CompleteHost(ProviderName, Request, MoveTemp(Result));
		}
	});
}

void UWebRTC4UnrealSubsystem::JoinSession(FName ProviderName,
	const FWebRTC4UnrealJoinRequest& Request)
{
	TSharedPtr<IWebRTC4UnrealProvider> Provider = GetOrCreateProvider(ProviderName);
	if (!Provider)
	{
		const FWebRTC4UnrealOperationResult Result = FWebRTC4UnrealOperationResult::Failure(
			TEXT("provider_not_found"),
			FString::Printf(TEXT("WebRTC4Unreal provider '%s' is not registered"),
				*ProviderName.ToString()));
		SetState(EWebRTC4UnrealConnectionState::Failed, Result.Message);
		OnOperationCompleted.Broadcast(Result);
		return;
	}

	ActiveProvider = ProviderName;
	SetState(EWebRTC4UnrealConnectionState::Resolving,
		FString::Printf(TEXT("Resolving room through %s"), *ProviderName.ToString()));
	TWeakObjectPtr<UWebRTC4UnrealSubsystem> WeakThis(this);
	Provider->Join(Request, [WeakThis, ProviderName](FWebRTC4UnrealOperationResult Result)
	{
		if (WeakThis.IsValid())
		{
			WeakThis->CompleteJoin(ProviderName, MoveTemp(Result));
		}
	});
}

void UWebRTC4UnrealSubsystem::LeaveSession()
{
	if (TSharedPtr<IWebRTC4UnrealProvider>* Provider = Providers.Find(ActiveProvider))
	{
		if (*Provider)
		{
			(*Provider)->Leave();
		}
	}
	CurrentSession = FWebRTC4UnrealSessionDescriptor();
	ActiveProvider = NAME_None;
	SetState(EWebRTC4UnrealConnectionState::Idle, TEXT("Session left"));
}

TArray<FName> UWebRTC4UnrealSubsystem::GetAvailableProviders() const
{
	return FWebRTC4UnrealProviderRegistry::GetProviderNames();
}

TSharedPtr<IWebRTC4UnrealProvider> UWebRTC4UnrealSubsystem::GetOrCreateProvider(FName ProviderName)
{
	if (TSharedPtr<IWebRTC4UnrealProvider>* Existing = Providers.Find(ProviderName))
	{
		return *Existing;
	}
	TSharedPtr<IWebRTC4UnrealProvider> Provider =
		FWebRTC4UnrealProviderRegistry::Create(ProviderName, *this);
	if (Provider)
	{
		if (const FWebRTC4UnrealProviderConfiguration* Configuration = Configurations.Find(ProviderName))
		{
			Provider->Configure(*Configuration);
		}
		Providers.Add(ProviderName, Provider);
	}
	return Provider;
}

void UWebRTC4UnrealSubsystem::SetState(EWebRTC4UnrealConnectionState NewState,
	const FString& Detail)
{
	if (State == NewState && Detail.IsEmpty())
	{
		return;
	}
	State = NewState;
	UE_LOG(LogWebRTC4UnrealSubsystem, Log, TEXT("WebRTC4Unreal state=%d detail=%s"),
		static_cast<int32>(State), *Detail);
	OnStateChanged.Broadcast(State, Detail);
}

void UWebRTC4UnrealSubsystem::CompleteHost(FName ProviderName,
	FWebRTC4UnrealHostRequest Request, FWebRTC4UnrealOperationResult Result)
{
	if (!Result.bSuccess)
	{
		SetState(EWebRTC4UnrealConnectionState::Failed, Result.Message);
		OnOperationCompleted.Broadcast(Result);
		UE_LOG(LogWebRTC4UnrealSubsystem, Error,
			TEXT("P2PTEST_FAIL stage=host code=%s message=%s"),
			*Result.ErrorCode, *Result.Message);
		return;
	}

	CurrentSession = Result.Session;
	CurrentSession.Provider = ProviderName;
	UWorld* World = GetWorld();
	if (!World)
	{
		if (!CurrentSession.TransportContextKey.IsEmpty())
		{
			FWebRTC4UnrealTransportRegistry::RemoveContext(CurrentSession.TransportContextKey);
		}
		Result = FWebRTC4UnrealOperationResult::Failure(TEXT("world_unavailable"),
			TEXT("No game world is available for listen travel"));
		SetState(EWebRTC4UnrealConnectionState::Failed, Result.Message);
		OnOperationCompleted.Broadcast(Result);
		return;
	}

	if (World->GetNetMode() == NM_ListenServer)
	{
		SetState(EWebRTC4UnrealConnectionState::Hosting, TEXT("Listen server already active"));
		OnOperationCompleted.Broadcast(Result);
		return;
	}

	FString TravelUrl = FString::Printf(TEXT("%s?listen"), *Request.MapPath);
	if (!CurrentSession.TransportName.IsNone())
	{
		if (CurrentSession.TransportContextKey.IsEmpty())
		{
			Result = FWebRTC4UnrealOperationResult::Failure(TEXT("transport_context_missing"),
				TEXT("Provider returned a custom transport without a context key"));
			SetState(EWebRTC4UnrealConnectionState::Failed, Result.Message);
			OnOperationCompleted.Broadcast(Result);
			return;
		}
		TravelUrl += FString::Printf(TEXT("?WebRTC4UnrealTransport=%s?WebRTC4UnrealContext=%s"),
			*CurrentSession.TransportName.ToString(), *CurrentSession.TransportContextKey);
	}
	else
	{
		TravelUrl += FString::Printf(TEXT("?Port=%d"), Request.ListenPort);
		World->URL.Port = Request.ListenPort;
	}

	SetState(EWebRTC4UnrealConnectionState::Traveling, TravelUrl);
	if (!World->ServerTravel(TravelUrl, true))
	{
		if (!CurrentSession.TransportContextKey.IsEmpty())
		{
			FWebRTC4UnrealTransportRegistry::RemoveContext(CurrentSession.TransportContextKey);
		}
		Result = FWebRTC4UnrealOperationResult::Failure(TEXT("listen_travel_failed"),
			FString::Printf(TEXT("ServerTravel failed for %s"), *TravelUrl));
		SetState(EWebRTC4UnrealConnectionState::Failed, Result.Message);
		OnOperationCompleted.Broadcast(Result);
		return;
	}
	OnOperationCompleted.Broadcast(Result);
}

void UWebRTC4UnrealSubsystem::CompleteJoin(FName ProviderName,
	FWebRTC4UnrealOperationResult Result)
{
	if (!Result.bSuccess)
	{
		SetState(EWebRTC4UnrealConnectionState::Failed, Result.Message);
		OnOperationCompleted.Broadcast(Result);
		UE_LOG(LogWebRTC4UnrealSubsystem, Error,
			TEXT("P2PTEST_FAIL stage=join code=%s message=%s"),
			*Result.ErrorCode, *Result.Message);
		return;
	}
	if (Result.Session.ConnectString.IsEmpty())
	{
		Result = FWebRTC4UnrealOperationResult::Failure(TEXT("missing_connect_string"),
			TEXT("Provider resolved the room without an Unreal connect string"));
		SetState(EWebRTC4UnrealConnectionState::Failed, Result.Message);
		OnOperationCompleted.Broadcast(Result);
		return;
	}

	CurrentSession = Result.Session;
	CurrentSession.Provider = ProviderName;
	UWorld* World = GetWorld();
	APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		if (!CurrentSession.TransportContextKey.IsEmpty())
		{
			FWebRTC4UnrealTransportRegistry::RemoveContext(CurrentSession.TransportContextKey);
		}
		Result = FWebRTC4UnrealOperationResult::Failure(TEXT("player_controller_unavailable"),
			TEXT("No local player controller is available for client travel"));
		SetState(EWebRTC4UnrealConnectionState::Failed, Result.Message);
		OnOperationCompleted.Broadcast(Result);
		return;
	}

	SetState(EWebRTC4UnrealConnectionState::Traveling, Result.Session.ConnectString);
	PlayerController->ClientTravel(Result.Session.ConnectString, TRAVEL_Absolute);
	OnOperationCompleted.Broadcast(Result);
}

void UWebRTC4UnrealSubsystem::TickAutomation(float DeltaTime)
{
	StartAutomationIfReady();
	if (bPassLogged && bAutomationExitRequested && PassSeconds > 0.0)
	{
		const double DwellSeconds = AutomationRole.Equals(TEXT("Host"), ESearchCase::IgnoreCase)
			? 10.0 : 7.0;
		if (FPlatformTime::Seconds() - PassSeconds >= DwellSeconds)
		{
			UE_LOG(LogWebRTC4UnrealSubsystem, Display, TEXT("P2PTEST_EXIT role=%s"),
				*AutomationRole);
			FPlatformMisc::RequestExit(false);
		}
	}
}

void UWebRTC4UnrealSubsystem::StartAutomationIfReady()
{
	if (bAutomationStarted || FPlatformTime::Seconds() - AutomationStartSeconds < 1.0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	bAutomationStarted = true;
	const FName ProviderName(*AutomationProvider);
	if (!AutomationProviderEndpoint.IsEmpty())
	{
		FWebRTC4UnrealProviderConfiguration Configuration;
		Configuration.Endpoint = AutomationProviderEndpoint;
		Configuration.Options.Add(TEXT("SessionBroker"), TEXT("true"));
		Configuration.Options.Add(TEXT("AccessKey"), AutomationAccessKey);
		if (FParse::Param(FCommandLine::Get(), TEXT("WebRTC4UnrealForceCloudflareFallback")))
		{
			Configuration.Options.Add(TEXT("ForceCloudflareFallback"), TEXT("true"));
		}
		FString FallbackTimeout;
		if (FParse::Value(FCommandLine::Get(),
			TEXT("WebRTC4UnrealFallbackTimeoutSeconds="), FallbackTimeout))
		{
			Configuration.Options.Add(TEXT("FallbackTimeoutSeconds"), FallbackTimeout);
		}
		ConfigureProvider(ProviderName, Configuration);
	}

	if (AutomationRole.Equals(TEXT("Host"), ESearchCase::IgnoreCase))
	{
		FWebRTC4UnrealHostRequest Request;
		Request.RoomName = AutomationRoom.IsEmpty() ? TEXT("WebRTC4Unreal test") : AutomationRoom;
		Request.MapPath = AutomationMap;
		Request.ListenPort = AutomationPort;
		Request.MaxParticipants = AutomationExpectedPlayers;
		Request.AdvertisedAddress = AutomationAdvertise.IsEmpty()
			? FString::Printf(TEXT("127.0.0.1:%d"), AutomationPort)
			: AutomationAdvertise;
		HostSession(ProviderName, Request);
	}
	else if (AutomationRole.Equals(TEXT("Client"), ESearchCase::IgnoreCase))
	{
		FWebRTC4UnrealJoinRequest Request;
		Request.RoomReference = ProviderName == FName(TEXT("DirectIp"))
			? AutomationConnect : AutomationRoom;
		JoinSession(ProviderName, Request);
	}
	else
	{
		UE_LOG(LogWebRTC4UnrealSubsystem, Error,
			TEXT("P2PTEST_FAIL stage=boot code=invalid_role message=%s"), *AutomationRole);
		SetState(EWebRTC4UnrealConnectionState::Failed,
			TEXT("P2PTestRole must be Host or Client"));
	}
}

void UWebRTC4UnrealSubsystem::TickConnectionDetection()
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	const ENetMode NetMode = World->GetNetMode();
	if (NetMode == NM_ListenServer)
	{
		if (bAutomationVerifyRPC)
		{
			EnsureAutomationReplicationProbes();
		}
		if (!bHostReadyLogged)
		{
			bHostReadyLogged = true;
			SetState(EWebRTC4UnrealConnectionState::Hosting,
				TEXT("Listen server is accepting clients"));
			UE_LOG(LogWebRTC4UnrealSubsystem, Display,
				TEXT("P2PTEST_HOST_LISTENING provider=%s room=%s expected_players=%d"),
				*ActiveProvider.ToString(), *CurrentSession.RoomId, AutomationExpectedPlayers);
		}

		int32 PlayerCount = 0;
		int32 PossessedPawnCount = 0;
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (APlayerController* PlayerController = It->Get())
			{
				++PlayerCount;
				if (PlayerController->GetPawn()) ++PossessedPawnCount;
			}
		}
		const int32 RequiredPlayers = bAutomationRequested ? AutomationExpectedPlayers : 2;
		const bool bRpcReady = !bAutomationVerifyRPC
			|| AreAutomationReplicationProbesReady();
		if (PlayerCount >= RequiredPlayers && PossessedPawnCount >= RequiredPlayers
			&& bRpcReady && !bPassLogged)
		{
			bPassLogged = true;
			PassSeconds = FPlatformTime::Seconds();
			SetState(EWebRTC4UnrealConnectionState::Connected,
				FString::Printf(TEXT("%d players connected"), PlayerCount));
			UE_LOG(LogWebRTC4UnrealSubsystem, Display,
				TEXT("P2PTEST_PASS_SERVER_PLAYERS=%d pawns=%d rpc=%d provider=%s room=%s"),
				PlayerCount, PossessedPawnCount, bAutomationVerifyRPC,
				*ActiveProvider.ToString(), *CurrentSession.RoomId);
		}
	}
	else if (NetMode == NM_Client)
	{
		UNetDriver* NetDriver = World->GetNetDriver();
		APlayerController* PlayerController = World->GetFirstPlayerController();
		const bool bRpcReady = !bAutomationVerifyRPC || IsClientReplicationProbeReady();
		if (NetDriver && NetDriver->ServerConnection && PlayerController
			&& PlayerController->GetPawn() && bRpcReady && !bPassLogged)
		{
			bPassLogged = true;
			PassSeconds = FPlatformTime::Seconds();
			SetState(EWebRTC4UnrealConnectionState::Connected,
				TEXT("Client connected to listen server"));
			UE_LOG(LogWebRTC4UnrealSubsystem, Display,
				TEXT("P2PTEST_PASS_CLIENT_CONNECTED rpc=%d provider=%s room=%s map=%s"),
				bAutomationVerifyRPC, *ActiveProvider.ToString(),
				*CurrentSession.RoomId, *World->GetMapName());
		}
	}
	else if (bAutomationRequested && bAutomationStarted
		&& State == EWebRTC4UnrealConnectionState::Traveling)
	{
		UE_LOG(LogWebRTC4UnrealSubsystem, VeryVerbose,
			TEXT("Waiting for network mode change; current=%s"), *NetModeToString(NetMode));
	}
}

void UWebRTC4UnrealSubsystem::EnsureAutomationReplicationProbes()
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() != NM_ListenServer)
	{
		return;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PlayerController = It->Get();
		if (!PlayerController || PlayerController->IsLocalController()
			|| AutomationReplicationProbes.Contains(PlayerController))
		{
			continue;
		}

		FActorSpawnParameters Parameters;
		Parameters.Owner = PlayerController;
		Parameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWebRTC4UnrealReplicationProbe* Probe = World->SpawnActor<
			AWebRTC4UnrealReplicationProbe>(Parameters);
		if (!Probe)
		{
			continue;
		}
		Probe->InitializeProbe(NextAutomationProbeId++);
		AutomationReplicationProbes.Add(PlayerController, Probe);
		UE_LOG(LogWebRTC4UnrealSubsystem, Display,
			TEXT("WEBRTC4UNREAL_RPC_PROBE_SPAWNED probe=%d remote=%s"),
			Probe->GetProbeId(), *PlayerController->GetName());
	}
}

bool UWebRTC4UnrealSubsystem::AreAutomationReplicationProbesReady() const
{
	const int32 RequiredRemotePlayers = FMath::Max(1, AutomationExpectedPlayers - 1);
	if (AutomationReplicationProbes.Num() < RequiredRemotePlayers)
	{
		return false;
	}
	for (const TPair<TWeakObjectPtr<APlayerController>,
		TWeakObjectPtr<AWebRTC4UnrealReplicationProbe>>& Pair : AutomationReplicationProbes)
	{
		if (!Pair.Key.IsValid() || !Pair.Value.IsValid()
			|| !Pair.Value->HasServerAcknowledgement())
		{
			return false;
		}
	}
	return true;
}

bool UWebRTC4UnrealSubsystem::IsClientReplicationProbeReady() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	for (TActorIterator<AWebRTC4UnrealReplicationProbe> It(World); It; ++It)
	{
		if (It->HasCompletedClientRoundTrip())
		{
			return true;
		}
	}
	return false;
}
