#pragma once

#include "CoreMinimal.h"
#include "IWebRTC4UnrealProvider.h"

class UWebRTC4UnrealSubsystem;

/** Flow-first provider whose Cloudflare Direct session is prepared as a hot fallback. */
class FFlowCloudflareFallbackProvider final : public IWebRTC4UnrealProvider
{
public:
	explicit FFlowCloudflareFallbackProvider(UWebRTC4UnrealSubsystem& InOwner)
		: Owner(InOwner)
	{
	}

	virtual FName GetProviderName() const override { return TEXT("FlowCloudflareFallback"); }
	virtual void Configure(const FWebRTC4UnrealProviderConfiguration& InConfiguration) override;
	virtual void Host(const FWebRTC4UnrealHostRequest& Request,
		FWebRTC4UnrealProviderCompletion Completion) override;
	virtual void Join(const FWebRTC4UnrealJoinRequest& Request,
		FWebRTC4UnrealProviderCompletion Completion) override;
	virtual void Leave() override;

private:
	bool Validate(FWebRTC4UnrealProviderCompletion& Completion);
	FWebRTC4UnrealProviderConfiguration ChildConfiguration(
		const FString& ParticipantId, const FString& RequestedRoomId = FString()) const;
	void FinishOperation(bool bHost, const FWebRTC4UnrealOperationResult& PrimaryResult,
		const FWebRTC4UnrealOperationResult& FallbackResult,
		FWebRTC4UnrealProviderCompletion Completion);

	UWebRTC4UnrealSubsystem& Owner;
	FWebRTC4UnrealProviderConfiguration Configuration;
	TSharedPtr<IWebRTC4UnrealProvider> FlowProvider;
	TSharedPtr<IWebRTC4UnrealProvider> CloudflareProvider;
	TSet<FString> ActiveContextKeys;
	bool bOperationInFlight = false;
};
