#include "WebRTC4UnrealDirectIpProvider.h"

void FWebRTC4UnrealDirectIpProvider::Host(const FWebRTC4UnrealHostRequest& Request, FWebRTC4UnrealProviderCompletion Completion)
{
	FWebRTC4UnrealSessionDescriptor Session;
	Session.Provider = GetProviderName();
	Session.RoomName = Request.RoomName;
	Session.ConnectString = Request.AdvertisedAddress;
	if (Session.ConnectString.IsEmpty())
	{
		Session.ConnectString = FString::Printf(TEXT("127.0.0.1:%d"), Request.ListenPort);
	}

	Completion(FWebRTC4UnrealOperationResult::Success(Session, TEXT("Direct listen endpoint resolved")));
}

void FWebRTC4UnrealDirectIpProvider::Join(const FWebRTC4UnrealJoinRequest& Request, FWebRTC4UnrealProviderCompletion Completion)
{
	if (Request.RoomReference.TrimStartAndEnd().IsEmpty())
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("invalid_address"), TEXT("DirectIp requires a host:port address")));
		return;
	}

	FWebRTC4UnrealSessionDescriptor Session;
	Session.Provider = GetProviderName();
	Session.ConnectString = Request.RoomReference.TrimStartAndEnd();
	Completion(FWebRTC4UnrealOperationResult::Success(Session, TEXT("Direct endpoint resolved")));
}
