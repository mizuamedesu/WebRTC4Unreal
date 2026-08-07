#include "FlowCloudflareFallbackProvider.h"

#include "FlowCloudflareFallbackTransport.h"
#include "IWebRTC4UnrealProvider.h"
#include "Misc/Guid.h"
#include "WebRTC4UnrealSubsystem.h"
#include "WebRTC4UnrealTransportRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlowCloudflareFallbackProvider, Log, All);

namespace
{
	const FName HybridTransportName(TEXT("FlowCloudflareFallback"));

	bool OptionEnabled(const TMap<FString, FString>& Options, const TCHAR* Name)
	{
		return Options.FindRef(Name).Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Options.FindRef(Name) == TEXT("1");
	}

	double FallbackTimeout(const TMap<FString, FString>& Options)
	{
		double Value = 8.0;
		LexTryParseString(Value, *Options.FindRef(TEXT("FallbackTimeoutSeconds")));
		return FMath::Clamp(Value, 2.0, 30.0);
	}
}

void FFlowCloudflareFallbackProvider::Configure(
	const FWebRTC4UnrealProviderConfiguration& InConfiguration)
{
	Configuration = InConfiguration;
	Configuration.Endpoint = Configuration.Endpoint.TrimStartAndEnd();
	while (Configuration.Endpoint.EndsWith(TEXT("/")))
	{
		Configuration.Endpoint.LeftChopInline(1, EAllowShrinking::No);
	}
}

bool FFlowCloudflareFallbackProvider::Validate(
	FWebRTC4UnrealProviderCompletion& Completion)
{
	if (bOperationInFlight)
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("hybrid_operation_in_progress"),
			TEXT("A Flow/Cloudflare session operation is already running")));
		return false;
	}
	if (Configuration.Endpoint.IsEmpty())
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("hybrid_endpoint_missing"),
			TEXT("Flow/Cloudflare Worker URL is empty")));
		return false;
	}
	if (Configuration.Options.FindRef(TEXT("AccessKey")).IsEmpty())
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("hybrid_access_key_missing"),
			TEXT("Flow/Cloudflare Worker bootstrap key is empty")));
		return false;
	}
	if (!FlowProvider)
	{
		FlowProvider = FWebRTC4UnrealProviderRegistry::Create(TEXT("Flow"), Owner);
	}
	if (!CloudflareProvider)
	{
		CloudflareProvider =
			FWebRTC4UnrealProviderRegistry::Create(TEXT("CloudflareDirect"), Owner);
	}
	if (!FlowProvider || !CloudflareProvider)
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("hybrid_provider_dependency_missing"),
			TEXT("Flow and Cloudflare Direct provider modules must both be enabled")));
		return false;
	}
	bOperationInFlight = true;
	return true;
}

FWebRTC4UnrealProviderConfiguration FFlowCloudflareFallbackProvider::ChildConfiguration(
	const FString& ParticipantId, const FString& RequestedRoomId) const
{
	FWebRTC4UnrealProviderConfiguration Child = Configuration;
	Child.Options.Add(TEXT("SessionBroker"), TEXT("true"));
	Child.Options.Add(TEXT("RequestedParticipantId"), ParticipantId);
	if (!RequestedRoomId.IsEmpty())
	{
		Child.Options.Add(TEXT("RequestedRoomId"), RequestedRoomId);
	}
	else
	{
		Child.Options.Remove(TEXT("RequestedRoomId"));
	}
	return Child;
}

void FFlowCloudflareFallbackProvider::Host(const FWebRTC4UnrealHostRequest& Request,
	FWebRTC4UnrealProviderCompletion Completion)
{
	if (!Validate(Completion)) return;
	FWebRTC4UnrealHostRequest HybridRequest = Request;
	HybridRequest.MaxParticipants = FMath::Clamp(HybridRequest.MaxParticipants, 2, 100);
	const FString ParticipantId =
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	UE_LOG(LogFlowCloudflareFallbackProvider, Display,
		TEXT("WEBRTC4UNREAL_HYBRID_IDENTITY role=host id=%s length=%d"),
		*ParticipantId, ParticipantId.Len());
	FlowProvider->Configure(ChildConfiguration(ParticipantId));
	FlowProvider->Host(HybridRequest,
		[this, HybridRequest, ParticipantId, Completion = MoveTemp(Completion)](
			FWebRTC4UnrealOperationResult PrimaryResult) mutable
		{
			const FString RequestedRoomId = PrimaryResult.bSuccess
				? PrimaryResult.Session.RoomId : FString();
			CloudflareProvider->Configure(ChildConfiguration(ParticipantId, RequestedRoomId));
			CloudflareProvider->Host(HybridRequest,
				[this, PrimaryResult = MoveTemp(PrimaryResult),
					Completion = MoveTemp(Completion)](
					FWebRTC4UnrealOperationResult FallbackResult) mutable
				{
					FinishOperation(true, PrimaryResult, FallbackResult, MoveTemp(Completion));
				});
		});
}

void FFlowCloudflareFallbackProvider::Join(const FWebRTC4UnrealJoinRequest& Request,
	FWebRTC4UnrealProviderCompletion Completion)
{
	if (!Validate(Completion)) return;
	const FString RoomId = Request.RoomReference.TrimStartAndEnd();
	if (RoomId.IsEmpty())
	{
		bOperationInFlight = false;
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("hybrid_room_missing"),
			TEXT("Paste the room ID shown by the host")));
		return;
	}
	const FString ParticipantId =
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	UE_LOG(LogFlowCloudflareFallbackProvider, Display,
		TEXT("WEBRTC4UNREAL_HYBRID_IDENTITY role=client id=%s length=%d"),
		*ParticipantId, ParticipantId.Len());
	FlowProvider->Configure(ChildConfiguration(ParticipantId));
	CloudflareProvider->Configure(ChildConfiguration(ParticipantId));
	FlowProvider->Join(Request,
		[this, Request, Completion = MoveTemp(Completion)](
			FWebRTC4UnrealOperationResult PrimaryResult) mutable
		{
			CloudflareProvider->Join(Request,
				[this, PrimaryResult = MoveTemp(PrimaryResult),
					Completion = MoveTemp(Completion)](
					FWebRTC4UnrealOperationResult FallbackResult) mutable
				{
					FinishOperation(false, PrimaryResult, FallbackResult, MoveTemp(Completion));
				});
		});
}

void FFlowCloudflareFallbackProvider::FinishOperation(bool bHost,
	const FWebRTC4UnrealOperationResult& PrimaryResult,
	const FWebRTC4UnrealOperationResult& FallbackResult,
	FWebRTC4UnrealProviderCompletion Completion)
{
	bOperationInFlight = false;
	const TSharedRef<FFlowCloudflareFallbackTransportContext> Context =
		MakeShared<FFlowCloudflareFallbackTransportContext>();

	auto ConsumeRegistration = [bHost](const FWebRTC4UnrealOperationResult& Result,
		FFlowCloudflareFallbackPath& Path) -> bool
	{
		if (!Result.bSuccess || Result.Session.TransportContextKey.IsEmpty()) return false;
		FWebRTC4UnrealTransportRegistration Registration;
		if (!FWebRTC4UnrealTransportRegistry::FindContext(
			Result.Session.TransportContextKey, Registration)
			|| !Registration.Context || Registration.bHost != bHost)
		{
			return false;
		}
		FWebRTC4UnrealTransportRegistry::RemoveContext(Result.Session.TransportContextKey);
		Path.TransportName = Registration.TransportName;
		Path.Context = MoveTemp(Registration.Context);
		return Path.IsAvailable();
	};

	const bool bPrimaryAvailable = ConsumeRegistration(PrimaryResult, Context->Primary);
	bool bFallbackAvailable = ConsumeRegistration(FallbackResult, Context->Fallback);
	if (bPrimaryAvailable && bFallbackAvailable
		&& PrimaryResult.Session.RoomId != FallbackResult.Session.RoomId)
	{
		UE_LOG(LogFlowCloudflareFallbackProvider, Error,
			TEXT("Hybrid provider received mismatched room IDs flow=%s cloudflare=%s"),
			*PrimaryResult.Session.RoomId, *FallbackResult.Session.RoomId);
		Context->Fallback = FFlowCloudflareFallbackPath();
		bFallbackAvailable = false;
	}

	Context->bForceFallback = OptionEnabled(
		Configuration.Options, TEXT("ForceCloudflareFallback"));
	Context->FallbackTimeoutSeconds = FallbackTimeout(Configuration.Options);
	if (!bPrimaryAvailable && !bFallbackAvailable)
	{
		const FString Message = FString::Printf(
			TEXT("Flow failed (%s); Cloudflare fallback failed (%s)"),
			PrimaryResult.Message.IsEmpty() ? *PrimaryResult.ErrorCode : *PrimaryResult.Message,
			FallbackResult.Message.IsEmpty() ? *FallbackResult.ErrorCode : *FallbackResult.Message);
		Completion(FWebRTC4UnrealOperationResult::Failure(
			TEXT("hybrid_all_paths_unavailable"), Message));
		return;
	}
	if (!bFallbackAvailable)
	{
		const FString Detail = FallbackResult.Message.IsEmpty()
			? FallbackResult.ErrorCode : FallbackResult.Message;
		Completion(FWebRTC4UnrealOperationResult::Failure(
			TEXT("hybrid_fallback_unavailable"),
			FString::Printf(TEXT("Cloudflare standby path could not be prepared: %s"),
				*Detail)));
		return;
	}
	const FWebRTC4UnrealSessionDescriptor& Source = bPrimaryAvailable
		? PrimaryResult.Session : FallbackResult.Session;
	Context->RoomId = Source.RoomId;
	const FString ContextKey =
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	if (!FWebRTC4UnrealTransportRegistry::RegisterContext(
		ContextKey, HybridTransportName, Context, bHost))
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(
			TEXT("transport_registration_failed"),
			TEXT("Could not register the Flow/Cloudflare fallback transport context")));
		return;
	}
	ActiveContextKeys.Add(ContextKey);

	FWebRTC4UnrealSessionDescriptor Session;
	Session.Provider = TEXT("FlowCloudflareFallback");
	Session.RoomId = Source.RoomId;
	Session.RoomName = Source.RoomName;
	Session.TransportName = HybridTransportName;
	Session.TransportContextKey = ContextKey;
	Session.MaxParticipants = Source.MaxParticipants;
	Session.bRelayOnly = false;
	Session.ConnectString = FString::Printf(
		TEXT("webrtc4unreal.invalid?WebRTC4UnrealTransport=%s?WebRTC4UnrealContext=%s"),
		*HybridTransportName.ToString(), *ContextKey);

	UE_LOG(LogFlowCloudflareFallbackProvider, Display,
		TEXT("WEBRTC4UNREAL_HYBRID_SESSION role=%s room=%s flow=%d cloudflare=%d timeout=%.1f forced=%d"),
		bHost ? TEXT("host") : TEXT("client"), *Session.RoomId,
		bPrimaryAvailable, bFallbackAvailable, Context->FallbackTimeoutSeconds,
		Context->bForceFallback);
	Completion(FWebRTC4UnrealOperationResult::Success(Session,
		bPrimaryAvailable
			? TEXT("Flow primary is ready with Cloudflare fallback")
			: TEXT("Flow is unavailable; Cloudflare fallback is ready")));
}

void FFlowCloudflareFallbackProvider::Leave()
{
	for (const FString& Key : ActiveContextKeys)
	{
		FWebRTC4UnrealTransportRegistry::RemoveContext(Key);
	}
	ActiveContextKeys.Reset();
	if (FlowProvider) FlowProvider->Leave();
	if (CloudflareProvider) CloudflareProvider->Leave();
	bOperationInFlight = false;
}
