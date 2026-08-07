#pragma once

#include "CoreMinimal.h"
#include "IWebRTC4UnrealProvider.h"

class FJsonObject;
class IHttpResponse;

class FCloudflareDirectProvider final : public IWebRTC4UnrealProvider
{
public:
	explicit FCloudflareDirectProvider(UWebRTC4UnrealSubsystem& InOwner) {}

	virtual FName GetProviderName() const override { return TEXT("CloudflareDirect"); }
	virtual void Configure(const FWebRTC4UnrealProviderConfiguration& InConfiguration) override;
	virtual void Host(const FWebRTC4UnrealHostRequest& Request,
		FWebRTC4UnrealProviderCompletion Completion) override;
	virtual void Join(const FWebRTC4UnrealJoinRequest& Request,
		FWebRTC4UnrealProviderCompletion Completion) override;
	virtual void Leave() override;

private:
	using FHttpCallback = TFunction<void(TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>, bool)>;

	bool ValidateConfiguration(FWebRTC4UnrealProviderCompletion& Completion) const;
	void SendWorkerRequest(const FString& Path, const FString& Verb,
		const TSharedPtr<FJsonObject>& Body, FHttpCallback Callback);
	bool ParseAndRegisterSession(const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
		bool bTransportSucceeded, bool bHost, FWebRTC4UnrealOperationResult& OutResult);
	static FWebRTC4UnrealOperationResult MakeHttpError(
		const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
		bool bTransportSucceeded);

	FWebRTC4UnrealProviderConfiguration Configuration;
	TSet<FString> ActiveContextKeys;
};
