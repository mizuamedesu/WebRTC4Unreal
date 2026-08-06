#include "Modules/ModuleManager.h"

#include "IWebRTC4UnrealProvider.h"
#include "WebRTC4UnrealDirectIpProvider.h"

DEFINE_LOG_CATEGORY_STATIC(LogWebRTC4UnrealCore, Log, All);

class FWebRTC4UnrealCoreModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FWebRTC4UnrealProviderRegistry::RegisterFactory(TEXT("DirectIp"), [](UWebRTC4UnrealSubsystem& Owner)
		{
			return MakeShared<FWebRTC4UnrealDirectIpProvider>(Owner);
		});
		UE_LOG(LogWebRTC4UnrealCore, Log, TEXT("WebRTC4Unreal core module started"));
	}

	virtual void ShutdownModule() override
	{
		FWebRTC4UnrealProviderRegistry::UnregisterFactory(TEXT("DirectIp"));
	}
};

IMPLEMENT_MODULE(FWebRTC4UnrealCoreModule, WebRTC4UnrealCore)
