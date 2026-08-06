#include "WebRTC4UnrealLobbySubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IPAddress.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "WebRTC4UnrealLobbyWidget.h"
#include "WebRTC4UnrealNetworkStatusWidget.h"
#include "WebRTC4UnrealSubsystem.h"
#include "SocketSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogWebRTC4UnrealLobbyUI, Log, All);

void UWebRTC4UnrealLobbySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	GConfig->GetBool(TEXT("WebRTC4Unreal"), TEXT("bShowLobbyOnStartup"), bEnabled, GGameIni);
	GConfig->GetBool(TEXT("WebRTC4Unreal"), TEXT("bShowNetworkStatus"), bNetworkStatusEnabled, GGameIni);
	GConfig->GetBool(TEXT("WebRTC4Unreal"), TEXT("bWriteDebugTextLog"), bWriteDebugTextLog, GGameIni);
	FString ProviderName = DefaultProvider.ToString();
	GConfig->GetString(TEXT("WebRTC4Unreal"), TEXT("DefaultProvider"), ProviderName, GGameIni);
	DefaultProvider = FName(*ProviderName);
	GConfig->GetString(TEXT("WebRTC4Unreal"), TEXT("ProviderEndpoint"), ProviderEndpoint, GGameIni);
	GConfig->GetString(TEXT("WebRTC4Unreal"), TEXT("ProviderAccessKey"),
		ProviderAccessKey, GGameIni);
	GConfig->GetString(TEXT("WebRTC4Unreal"), TEXT("AdvertisedAddress"), AdvertisedAddress, GGameIni);
	GConfig->GetInt(TEXT("WebRTC4Unreal"), TEXT("ListenPort"), ListenPort, GGameIni);
	GConfig->GetInt(TEXT("WebRTC4Unreal"), TEXT("MaxParticipants"), MaxParticipants, GGameIni);
	ListenPort = FMath::Clamp(ListenPort, 1, 65535);
	MaxParticipants = FMath::Clamp(MaxParticipants, 2, 1000);

	const TCHAR* CommandLine = FCommandLine::Get();
	FString AutomationRole;
	const bool bAutomation = FParse::Value(CommandLine, TEXT("P2PTestRole="), AutomationRole);
	const bool bKeepAutomationUi = FParse::Param(CommandLine, TEXT("P2PTestKeepUI"));
	if ((bAutomation && !bKeepAutomationUi) || FParse::Param(CommandLine, TEXT("NoP2PLobbyUI")))
	{
		bEnabled = false;
	}
	if ((bAutomation && !bKeepAutomationUi) || FParse::Param(CommandLine, TEXT("NoP2PNetworkStatus")))
	{
		bNetworkStatusEnabled = false;
	}
	FString ProviderOverride;
	if (FParse::Value(CommandLine, TEXT("WebRTC4UnrealProvider="), ProviderOverride)
		|| FParse::Value(CommandLine, TEXT("P2PProvider="), ProviderOverride))
	{
		DefaultProvider = FName(*ProviderOverride);
	}
	FParse::Value(CommandLine, TEXT("WebRTC4UnrealEndpoint="), ProviderEndpoint);
	FParse::Value(CommandLine, TEXT("WebRTC4UnrealAccessKey="), ProviderAccessKey);
	FParse::Value(CommandLine, TEXT("P2PAdvertise="), AdvertisedAddress);
	FParse::Value(CommandLine, TEXT("P2PPort="), ListenPort);
	FParse::Value(CommandLine, TEXT("WebRTC4UnrealMaxParticipants="), MaxParticipants);

	StartDebugTextLog();

	if (AdvertisedAddress.IsEmpty())
	{
		AdvertisedAddress = DetectAdvertisedAddress();
	}
}

void UWebRTC4UnrealLobbySubsystem::Deinitialize()
{
	if (LobbyWidget.IsValid())
	{
		LobbyWidget->RemoveFromParent();
	}
	if (NetworkStatusWidget.IsValid())
	{
		NetworkStatusWidget->RemoveFromParent();
	}
	LobbyWidget.Reset();
	NetworkStatusWidget.Reset();
	if (DebugTextLog)
	{
		DebugTextLog->Flush();
		if (GLog)
		{
			GLog->RemoveOutputDevice(DebugTextLog.Get());
		}
		DebugTextLog.Reset();
	}
	Super::Deinitialize();
}

void UWebRTC4UnrealLobbySubsystem::StartDebugTextLog()
{
	if (!bWriteDebugTextLog || DebugTextLog || !GLog)
	{
		return;
	}

	const FString LogDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
	IFileManager::Get().MakeDirectory(*LogDirectory, true);
	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	const FString DebugLogPath = FPaths::Combine(LogDirectory,
		FString::Printf(TEXT("WebRTC4Unreal-%s-%u.txt"), *Timestamp, FPlatformProcess::GetCurrentProcessId()));
	DebugTextLog = MakeUnique<FOutputDeviceFile>(*DebugLogPath, true, false, false);
	GLog->AddOutputDevice(DebugTextLog.Get());
	UE_LOG(LogWebRTC4UnrealLobbyUI, Display, TEXT("WEBRTC4UNREAL_DEBUG_TEXT_LOG path=%s"), *DebugLogPath);
}

void UWebRTC4UnrealLobbySubsystem::Tick(float DeltaTime)
{
	if (bNetworkStatusEnabled && (!NetworkStatusWidget.IsValid() || !NetworkStatusWidget->IsInViewport()))
	{
		TryCreateNetworkStatus();
	}
	if (NetworkStatusWidget.IsValid())
	{
		NetworkStatusWidget->Refresh();
	}
	if (bEnabled && bShouldShow && !LobbyWidget.IsValid())
	{
		TryCreateLobby();
	}
	if (bShouldShow && LobbyWidget.IsValid() && HasConnectedRemoteClient())
	{
		if (!bHostAutoHideLogged)
		{
			bHostAutoHideLogged = true;
			UE_LOG(LogWebRTC4UnrealLobbyUI, Display, TEXT("WEBRTC4UNREAL_LOBBY_HOST_AUTO_HIDDEN reason=remote_connection_open"));
		}
		HideLobby();
	}
}

UWorld* UWebRTC4UnrealLobbySubsystem::GetTickableGameObjectWorld() const
{
	return GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
}

void UWebRTC4UnrealLobbySubsystem::ShowLobby()
{
	bEnabled = true;
	bShouldShow = true;
	if (LobbyWidget.IsValid())
	{
		if (!LobbyWidget->IsInViewport())
		{
			LobbyWidget->AddToViewport(1000);
		}
		LobbyWidget->SetVisibility(ESlateVisibility::Visible);
		ApplyLobbyInputMode(true);
	}
	else
	{
		TryCreateLobby();
	}
}

void UWebRTC4UnrealLobbySubsystem::HideLobby()
{
	bShouldShow = false;
	if (LobbyWidget.IsValid())
	{
		LobbyWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
	ApplyLobbyInputMode(false);
}

void UWebRTC4UnrealLobbySubsystem::TryCreateLobby()
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	APlayerController* PlayerController = World->GetFirstPlayerController();
	if (!PlayerController || !PlayerController->IsLocalController())
	{
		return;
	}

	UWebRTC4UnrealLobbyWidget* Widget = CreateWidget<UWebRTC4UnrealLobbyWidget>(PlayerController, UWebRTC4UnrealLobbyWidget::StaticClass());
	if (!Widget)
	{
		return;
	}
	Widget->SetDefaults(DefaultProvider, ProviderEndpoint, ProviderAccessKey,
		AdvertisedAddress, ListenPort, MaxParticipants);
	Widget->AddToViewport(1000);
	LobbyWidget = Widget;
	ApplyLobbyInputMode(true);
}

void UWebRTC4UnrealLobbySubsystem::TryCreateNetworkStatus()
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	APlayerController* PlayerController = World->GetFirstPlayerController();
	if (!PlayerController || !PlayerController->IsLocalController())
	{
		return;
	}
	if (NetworkStatusWidget.IsValid())
	{
		if (!NetworkStatusWidget->IsInViewport())
		{
			NetworkStatusWidget->AddToViewport(900);
		}
		return;
	}

	UWebRTC4UnrealNetworkStatusWidget* Widget = CreateWidget<UWebRTC4UnrealNetworkStatusWidget>(
		PlayerController, UWebRTC4UnrealNetworkStatusWidget::StaticClass());
	if (Widget)
	{
		Widget->AddToViewport(900);
		NetworkStatusWidget = Widget;
	}
}

bool UWebRTC4UnrealLobbySubsystem::HasConnectedRemoteClient() const
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() != NM_ListenServer)
	{
		return false;
	}
	UNetDriver* NetDriver = World->GetNetDriver();
	if (!NetDriver)
	{
		return false;
	}
	for (const UNetConnection* Connection : NetDriver->ClientConnections)
	{
		if (Connection && Connection->GetConnectionState() == USOCK_Open)
		{
			return true;
		}
	}
	return false;
}

FString UWebRTC4UnrealLobbySubsystem::DetectAdvertisedAddress() const
{
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem)
	{
		TArray<TSharedPtr<FInternetAddr>> Addresses;
		if (SocketSubsystem->GetLocalAdapterAddresses(Addresses))
		{
			for (const TSharedPtr<FInternetAddr>& Address : Addresses)
			{
				if (!Address || !Address->IsValid())
				{
					continue;
				}
				const FString Ip = Address->ToString(false);
				if (!Ip.IsEmpty() && !Ip.StartsWith(TEXT("127.")) && Ip != TEXT("0.0.0.0") && !Ip.Contains(TEXT(":")))
				{
					return FString::Printf(TEXT("%s:%d"), *Ip, ListenPort);
				}
			}
		}
	}
	return FString::Printf(TEXT("127.0.0.1:%d"), ListenPort);
}

void UWebRTC4UnrealLobbySubsystem::ApplyLobbyInputMode(bool bLobbyVisible) const
{
	UWorld* World = GetWorld();
	APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		return;
	}

	PlayerController->SetShowMouseCursor(bLobbyVisible);
	if (bLobbyVisible && LobbyWidget.IsValid())
	{
		FInputModeGameAndUI InputMode;
		InputMode.SetWidgetToFocus(LobbyWidget->TakeWidget());
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PlayerController->SetInputMode(InputMode);
	}
	else
	{
		PlayerController->SetInputMode(FInputModeGameOnly());
	}
}
