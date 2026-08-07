#include "Modules/ModuleManager.h"

#include "CloudflareDirectProvider.h"
#include "CloudflareDirectTransport.h"
#include "CloudflareRealtimeProvider.h"
#include "CloudflareRealtimeTransport.h"
#include "FlowCloudflareFallbackProvider.h"
#include "FlowCloudflareFallbackTransport.h"
#include "IWebRTC4UnrealProvider.h"
#include "WebRTC4UnrealTransportRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogWebRTC4UnrealCloudflareRealtime, Log, All);

class FWebRTC4UnrealCloudflareRealtimeModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FWebRTC4UnrealProviderRegistry::RegisterFactory(TEXT("CloudflareRealtime"),
			[](UWebRTC4UnrealSubsystem& Owner)
			{
				return StaticCastSharedRef<IWebRTC4UnrealProvider>(
					MakeShared<FCloudflareRealtimeProvider>(Owner));
			});
		FWebRTC4UnrealTransportRegistry::RegisterFactory(TEXT("CloudflareRealtime"), []()
		{
			return StaticCastSharedRef<IWebRTC4UnrealTransportEndpoint>(
				MakeShared<FCloudflareRealtimeTransport>());
		});
		FWebRTC4UnrealProviderRegistry::RegisterFactory(TEXT("CloudflareDirect"),
			[](UWebRTC4UnrealSubsystem& Owner)
			{
				return StaticCastSharedRef<IWebRTC4UnrealProvider>(
					MakeShared<FCloudflareDirectProvider>(Owner));
			});
		FWebRTC4UnrealTransportRegistry::RegisterFactory(TEXT("CloudflareDirect"), []()
		{
			return StaticCastSharedRef<IWebRTC4UnrealTransportEndpoint>(
				MakeShared<FCloudflareDirectTransport>());
		});
		FWebRTC4UnrealProviderRegistry::RegisterFactory(TEXT("FlowCloudflareFallback"),
			[](UWebRTC4UnrealSubsystem& Owner)
			{
				return StaticCastSharedRef<IWebRTC4UnrealProvider>(
					MakeShared<FFlowCloudflareFallbackProvider>(Owner));
			});
		FWebRTC4UnrealTransportRegistry::RegisterFactory(TEXT("FlowCloudflareFallback"), []()
		{
			return StaticCastSharedRef<IWebRTC4UnrealTransportEndpoint>(
				MakeShared<FFlowCloudflareFallbackTransport>());
		});
		UE_LOG(LogWebRTC4UnrealCloudflareRealtime, Log,
			TEXT("Cloudflare Realtime, Direct P2P, and Flow-first fallback providers registered"));
	}

	virtual void ShutdownModule() override
	{
		FWebRTC4UnrealProviderRegistry::UnregisterFactory(TEXT("CloudflareRealtime"));
		FWebRTC4UnrealTransportRegistry::UnregisterFactory(TEXT("CloudflareRealtime"));
		FWebRTC4UnrealProviderRegistry::UnregisterFactory(TEXT("CloudflareDirect"));
		FWebRTC4UnrealTransportRegistry::UnregisterFactory(TEXT("CloudflareDirect"));
		FWebRTC4UnrealProviderRegistry::UnregisterFactory(TEXT("FlowCloudflareFallback"));
		FWebRTC4UnrealTransportRegistry::UnregisterFactory(TEXT("FlowCloudflareFallback"));
	}
};

IMPLEMENT_MODULE(FWebRTC4UnrealCloudflareRealtimeModule, WebRTC4UnrealCloudflareRealtime)
