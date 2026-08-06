#include "Modules/ModuleManager.h"

#include "FlowWebRTCProvider.h"
#include "FlowWebRTCTransport.h"
#include "IWebRTC4UnrealProvider.h"
#include "WebRTC4UnrealTransportRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogWebRTC4UnrealFlow, Log, All);

class FWebRTC4UnrealFlowModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FWebRTC4UnrealProviderRegistry::RegisterFactory(TEXT("Flow"), [](UWebRTC4UnrealSubsystem& Owner)
		{
			return StaticCastSharedRef<IWebRTC4UnrealProvider>(MakeShared<FFlowWebRTCProvider>(Owner));
		});
		FWebRTC4UnrealTransportRegistry::RegisterFactory(TEXT("FlowWebRTC"), []()
		{
			return StaticCastSharedRef<IWebRTC4UnrealTransportEndpoint>(
				MakeShared<FFlowWebRTCTransport>());
		});
		UE_LOG(LogWebRTC4UnrealFlow, Log,
			TEXT("WebRTC4Unreal Flow provider and multi-peer transport registered"));
	}

	virtual void ShutdownModule() override
	{
		FWebRTC4UnrealProviderRegistry::UnregisterFactory(TEXT("Flow"));
		FWebRTC4UnrealTransportRegistry::UnregisterFactory(TEXT("FlowWebRTC"));
	}
};

IMPLEMENT_MODULE(FWebRTC4UnrealFlowModule, WebRTC4UnrealFlow)
