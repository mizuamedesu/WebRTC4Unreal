#include "FlowBackendSubsystem.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlowBackend, Log, All);

#ifndef WEBRTC4UNREAL_WITH_EMBEDDED_DEVELOPER_CREDENTIAL
#define WEBRTC4UNREAL_WITH_EMBEDDED_DEVELOPER_CREDENTIAL 0
#endif

void UFlowBackendSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	GConfig->GetBool(TEXT("WebRTC4UnrealFlow"), TEXT("bAutoStartBundledBackend"),
		bAutoStartBundledBackend, GGameIni);
	GConfig->GetInt(TEXT("WebRTC4UnrealFlow"), TEXT("BundledBackendPort"),
		BundledBackendPort, GGameIni);
	GConfig->GetString(TEXT("WebRTC4UnrealFlow"), TEXT("HostPrincipalId"),
		HostPrincipalId, GGameIni);
	BundledBackendPort = FMath::Clamp(BundledBackendPort, 1, 65535);

	const TCHAR* CommandLine = FCommandLine::Get();
	FParse::Value(CommandLine, TEXT("WebRTC4UnrealBundledBackendPort="), BundledBackendPort);
	FParse::Value(CommandLine, TEXT("P2PBundledBackendPort="), BundledBackendPort);
	FParse::Value(CommandLine, TEXT("WebRTC4UnrealFlowHostPrincipalId="),
		HostPrincipalId);
	FString ExternalEndpoint;
	const bool bExternalEndpoint =
		FParse::Value(CommandLine, TEXT("WebRTC4UnrealEndpoint="), ExternalEndpoint);
	if (bExternalEndpoint || FParse::Param(CommandLine, TEXT("NoWebRTC4UnrealBundledBackend"))
		|| FParse::Param(CommandLine, TEXT("NoP2PBundledBackend")))
	{
		bAutoStartBundledBackend = false;
	}

	if (bAutoStartBundledBackend && !IsRunningCommandlet())
	{
		StartBundledBackend();
	}
}

void UFlowBackendSubsystem::Deinitialize()
{
	ReleaseBundledBackendHandle();
	Super::Deinitialize();
}

void UFlowBackendSubsystem::StartBundledBackend()
{
#if PLATFORM_WINDOWS && !UE_BUILD_SHIPPING && WEBRTC4UNREAL_WITH_EMBEDDED_DEVELOPER_CREDENTIAL
	const FString Credential = UTF8_TO_TCHAR(WEBRTC4UNREAL_EMBEDDED_DEVELOPER_CREDENTIAL);
	if (Credential.IsEmpty())
	{
		UE_LOG(LogFlowBackend, Error,
			TEXT("WEBRTC4UNREAL_BUNDLED_BACKEND_FAILED reason=embedded_credential_empty"));
		return;
	}
	FGuid ParsedPrincipalId;
	if (!FGuid::Parse(HostPrincipalId, ParsedPrincipalId))
	{
		UE_LOG(LogFlowBackend, Error,
			TEXT("WEBRTC4UNREAL_BUNDLED_BACKEND_FAILED reason=host_principal_id_invalid"));
		return;
	}

	const FString BackendDirectory = FPaths::Combine(
		FPlatformProcess::BaseDir(), TEXT("WebRTC4UnrealBackend"));
	const FString NodePath = FPaths::Combine(BackendDirectory, TEXT("node.exe"));
	const FString ServerPath = FPaths::Combine(BackendDirectory, TEXT("server.js"));
	if (!IFileManager::Get().FileExists(*NodePath)
		|| !IFileManager::Get().FileExists(*ServerPath))
	{
		UE_LOG(LogFlowBackend, Error,
			TEXT("WEBRTC4UNREAL_BUNDLED_BACKEND_FAILED reason=runtime_missing node=%d server=%d directory=%s"),
			IFileManager::Get().FileExists(*NodePath),
			IFileManager::Get().FileExists(*ServerPath), *BackendDirectory);
		return;
	}

	const FString LogDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
	IFileManager::Get().MakeDirectory(*LogDirectory, true);
	const FString BackendLogPath = FPaths::Combine(LogDirectory,
		FString::Printf(TEXT("WebRTC4UnrealBackend-%u.txt"),
			FPlatformProcess::GetCurrentProcessId()));
	const FString ParentProcessId = LexToString(FPlatformProcess::GetCurrentProcessId());
	const FString BackendPort = LexToString(BundledBackendPort);

	const TCHAR* CredentialVariable = TEXT("HCF_DEVELOPER_CREDENTIAL");
	const TCHAR* PrincipalVariable = TEXT("HCF_PRINCIPAL_ID");
	const TCHAR* BindVariable = TEXT("P2P_BIND_HOST");
	const TCHAR* PortVariable = TEXT("P2P_BACKEND_PORT");
	const TCHAR* BootstrapVariable = TEXT("P2P_BOOTSTRAP_KEY");
	const TCHAR* LogVariable = TEXT("P2P_BACKEND_LOG_FILE");
	const TCHAR* ParentVariable = TEXT("P2P_BACKEND_PARENT_PID");
	const FString OldCredential = FPlatformMisc::GetEnvironmentVariable(CredentialVariable);
	const FString OldPrincipal = FPlatformMisc::GetEnvironmentVariable(PrincipalVariable);
	const FString OldBind = FPlatformMisc::GetEnvironmentVariable(BindVariable);
	const FString OldPort = FPlatformMisc::GetEnvironmentVariable(PortVariable);
	const FString OldBootstrap = FPlatformMisc::GetEnvironmentVariable(BootstrapVariable);
	const FString OldLog = FPlatformMisc::GetEnvironmentVariable(LogVariable);
	const FString OldParent = FPlatformMisc::GetEnvironmentVariable(ParentVariable);

	FPlatformMisc::SetEnvironmentVar(CredentialVariable, *Credential);
	FPlatformMisc::SetEnvironmentVar(PrincipalVariable, *HostPrincipalId);
	FPlatformMisc::SetEnvironmentVar(BindVariable, TEXT("127.0.0.1"));
	FPlatformMisc::SetEnvironmentVar(PortVariable, *BackendPort);
	FPlatformMisc::SetEnvironmentVar(BootstrapVariable, TEXT(""));
	FPlatformMisc::SetEnvironmentVar(LogVariable, *BackendLogPath);
	FPlatformMisc::SetEnvironmentVar(ParentVariable, *ParentProcessId);

	const FString Arguments = FString::Printf(TEXT("\"%s\""), *ServerPath);
	BundledBackendProcess = FPlatformProcess::CreateProc(*NodePath, *Arguments,
		true, true, true, &BundledBackendProcessId, 0, *BackendDirectory,
		nullptr, nullptr);

	FPlatformMisc::SetEnvironmentVar(CredentialVariable, *OldCredential);
	FPlatformMisc::SetEnvironmentVar(PrincipalVariable, *OldPrincipal);
	FPlatformMisc::SetEnvironmentVar(BindVariable, *OldBind);
	FPlatformMisc::SetEnvironmentVar(PortVariable, *OldPort);
	FPlatformMisc::SetEnvironmentVar(BootstrapVariable, *OldBootstrap);
	FPlatformMisc::SetEnvironmentVar(LogVariable, *OldLog);
	FPlatformMisc::SetEnvironmentVar(ParentVariable, *OldParent);

	if (BundledBackendProcess.IsValid())
	{
		UE_LOG(LogFlowBackend, Display,
			TEXT("WEBRTC4UNREAL_BUNDLED_BACKEND_STARTED pid=%u endpoint=http://127.0.0.1:%d log=%s"),
			BundledBackendProcessId, BundledBackendPort, *BackendLogPath);
	}
	else
	{
		UE_LOG(LogFlowBackend, Error,
			TEXT("WEBRTC4UNREAL_BUNDLED_BACKEND_FAILED reason=create_process_failed"));
	}
#else
	UE_LOG(LogFlowBackend, Error,
		TEXT("WEBRTC4UNREAL_BUNDLED_BACKEND_FAILED reason=embedded_backend_not_available_in_this_build"));
#endif
}

void UFlowBackendSubsystem::ReleaseBundledBackendHandle()
{
	if (BundledBackendProcess.IsValid())
	{
		FPlatformProcess::CloseProc(BundledBackendProcess);
		BundledBackendProcess.Reset();
	}
	BundledBackendProcessId = 0;
}
