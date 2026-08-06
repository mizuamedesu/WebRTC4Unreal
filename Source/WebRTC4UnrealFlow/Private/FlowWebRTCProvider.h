#pragma once

#include "CoreMinimal.h"
#include "IWebRTC4UnrealProvider.h"

class FJsonObject;
class IHttpResponse;

class FFlowWebRTCProvider final : public IWebRTC4UnrealProvider
{
public:
	explicit FFlowWebRTCProvider(UWebRTC4UnrealSubsystem& InOwner) {}

	virtual FName GetProviderName() const override { return TEXT("Flow"); }
	virtual void Configure(const FWebRTC4UnrealProviderConfiguration& InConfiguration) override;
	virtual void Host(const FWebRTC4UnrealHostRequest& Request,
		FWebRTC4UnrealProviderCompletion Completion) override;
	virtual void Join(const FWebRTC4UnrealJoinRequest& Request,
		FWebRTC4UnrealProviderCompletion Completion) override;
	virtual void Leave() override;

private:
	using FHttpCallback = TFunction<void(TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>, bool)>;

	bool ValidateConfiguration(FWebRTC4UnrealProviderCompletion& Completion) const;
	bool IsBackendSessionBroker() const;
	void HostThroughBackend(const FWebRTC4UnrealHostRequest& Request,
		FWebRTC4UnrealProviderCompletion Completion);
	void JoinThroughBackend(const FWebRTC4UnrealJoinRequest& Request,
		FWebRTC4UnrealProviderCompletion Completion);
	void SendBackendRequest(const FString& Path, FHttpCallback Callback);
	bool ParseAndRegisterBackendSession(
		const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
		bool bTransportSucceeded, bool bHost, FWebRTC4UnrealOperationResult& OutResult);

	static bool ParseObjectResponse(
		const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
		bool bTransportSucceeded, const TArray<int32>& ExpectedCodes,
		TSharedPtr<FJsonObject>& OutObject, FWebRTC4UnrealOperationResult& OutError);
	static FWebRTC4UnrealOperationResult MakeHttpError(
		const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
		bool bTransportSucceeded);
	static void ReadStringArray(const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName, TArray<FString>& OutValues);

	FWebRTC4UnrealProviderConfiguration Configuration;
	TSet<FString> ActiveContextKeys;
};
