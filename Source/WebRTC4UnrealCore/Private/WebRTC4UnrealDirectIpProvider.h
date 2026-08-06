#pragma once

#include "CoreMinimal.h"
#include "IWebRTC4UnrealProvider.h"

class FWebRTC4UnrealDirectIpProvider final : public IWebRTC4UnrealProvider
{
public:
	explicit FWebRTC4UnrealDirectIpProvider(UWebRTC4UnrealSubsystem& InOwner) {}

	virtual FName GetProviderName() const override { return TEXT("DirectIp"); }
	virtual void Configure(const FWebRTC4UnrealProviderConfiguration& InConfiguration) override { Configuration = InConfiguration; }
	virtual void Host(const FWebRTC4UnrealHostRequest& Request, FWebRTC4UnrealProviderCompletion Completion) override;
	virtual void Join(const FWebRTC4UnrealJoinRequest& Request, FWebRTC4UnrealProviderCompletion Completion) override;
	virtual void Leave() override {}

private:
	FWebRTC4UnrealProviderConfiguration Configuration;
};
