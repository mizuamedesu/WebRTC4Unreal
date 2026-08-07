#include "FlowWebRTCProvider.h"

#include "Dom/JsonObject.h"
#include "FlowWebRTCTransport.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "WebRTC4UnrealTransportRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlowWebRTCProvider, Log, All);

namespace
{
	const FName FlowTransportName(TEXT("FlowWebRTC"));

	FString GetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		FString Value;
		if (Object) Object->TryGetStringField(FieldName, Value);
		return Value;
	}
}

void FFlowWebRTCProvider::Configure(
	const FWebRTC4UnrealProviderConfiguration& InConfiguration)
{
	Configuration = InConfiguration;
	Configuration.Endpoint = Configuration.Endpoint.TrimStartAndEnd();
	while (Configuration.Endpoint.EndsWith(TEXT("/")))
	{
		Configuration.Endpoint.LeftChopInline(1, EAllowShrinking::No);
	}
}

bool FFlowWebRTCProvider::ValidateConfiguration(
	FWebRTC4UnrealProviderCompletion& Completion) const
{
	if (Configuration.Endpoint.IsEmpty())
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("flow_configuration_missing"),
			TEXT("Trusted Flow session broker URL is empty")));
		return false;
	}
	if (!IsBackendSessionBroker())
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("flow_backend_required"),
			TEXT("Flow requires a trusted backend session broker")));
		return false;
	}
	return true;
}

bool FFlowWebRTCProvider::IsBackendSessionBroker() const
{
	const FString BrokerOption = Configuration.Options.Contains(TEXT("SessionBroker"))
		? Configuration.Options.FindRef(TEXT("SessionBroker"))
		: Configuration.Options.FindRef(TEXT("BackendSessionBroker"));
	return BrokerOption.Equals(TEXT("true"), ESearchCase::IgnoreCase);
}

void FFlowWebRTCProvider::Host(const FWebRTC4UnrealHostRequest& Request,
	FWebRTC4UnrealProviderCompletion Completion)
{
	if (ValidateConfiguration(Completion))
	{
		HostThroughBackend(Request, MoveTemp(Completion));
	}
}

void FFlowWebRTCProvider::Join(const FWebRTC4UnrealJoinRequest& Request,
	FWebRTC4UnrealProviderCompletion Completion)
{
	if (ValidateConfiguration(Completion))
	{
		JoinThroughBackend(Request, MoveTemp(Completion));
	}
}

void FFlowWebRTCProvider::Leave()
{
	for (const FString& Key : ActiveContextKeys)
	{
		FWebRTC4UnrealTransportRegistry::RemoveContext(Key);
	}
	ActiveContextKeys.Reset();
}

void FFlowWebRTCProvider::HostThroughBackend(const FWebRTC4UnrealHostRequest& Request,
	FWebRTC4UnrealProviderCompletion Completion)
{
	const FString Path = FString::Printf(
		TEXT("/api/p2p/sessions/host?room_name=%s&max_participants=%d"),
		*FGenericPlatformHttp::UrlEncode(Request.RoomName),
		FMath::Clamp(Request.MaxParticipants, 2, 1000));
	SendBackendRequest(Path,
		[this, Completion = MoveTemp(Completion)](
			TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded) mutable
		{
			FWebRTC4UnrealOperationResult Result;
			ParseAndRegisterBackendSession(Response, bSucceeded, true, Result);
			Completion(MoveTemp(Result));
		});
}

void FFlowWebRTCProvider::JoinThroughBackend(const FWebRTC4UnrealJoinRequest& Request,
	FWebRTC4UnrealProviderCompletion Completion)
{
	const FString RoomId = Request.RoomReference.TrimStartAndEnd();
	if (RoomId.IsEmpty())
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("flow_room_missing"),
			TEXT("Paste the room ID shown by the host")));
		return;
	}
	const FString Path = FString::Printf(TEXT("/api/p2p/sessions/join?room_id=%s"),
		*FGenericPlatformHttp::UrlEncode(RoomId));
	SendBackendRequest(Path,
		[this, Completion = MoveTemp(Completion)](
			TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded) mutable
		{
			FWebRTC4UnrealOperationResult Result;
			ParseAndRegisterBackendSession(Response, bSucceeded, false, Result);
			Completion(MoveTemp(Result));
		});
}

void FFlowWebRTCProvider::SendBackendRequest(const FString& Path, FHttpCallback Callback)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		FHttpModule::Get().CreateRequest();
	const TSharedRef<FHttpCallback, ESPMode::ThreadSafe> SharedCallback =
		MakeShared<FHttpCallback, ESPMode::ThreadSafe>(MoveTemp(Callback));
	Request->SetURL(Configuration.Endpoint + Path);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Cache-Control"), TEXT("no-store"));
	const FString BootstrapKey = Configuration.Options.FindRef(TEXT("AccessKey"));
	if (!BootstrapKey.IsEmpty())
	{
		Request->SetHeader(TEXT("X-P2P-Bootstrap-Key"), BootstrapKey);
	}
	const FString RequestedParticipantId =
		Configuration.Options.FindRef(TEXT("RequestedParticipantId"));
	if (!RequestedParticipantId.IsEmpty())
	{
		Request->SetHeader(TEXT("X-WebRTC4Unreal-Participant-Id"), RequestedParticipantId);
	}
	Request->OnProcessRequestComplete().BindLambda(
		[SharedCallback](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			(*SharedCallback)(Response, bSucceeded);
		});
	if (!Request->ProcessRequest())
	{
		(*SharedCallback)(nullptr, false);
	}
}

bool FFlowWebRTCProvider::ParseAndRegisterBackendSession(
	const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
	bool bTransportSucceeded, bool bHost, FWebRTC4UnrealOperationResult& OutResult)
{
	TSharedPtr<FJsonObject> Json;
	FWebRTC4UnrealOperationResult HttpError;
	if (!ParseObjectResponse(Response, bTransportSucceeded, { 200 }, Json, HttpError))
	{
		OutResult = MoveTemp(HttpError);
		return false;
	}
	bool bRelayOnly = false;
	Json->TryGetBoolField(TEXT("relay_only"), bRelayOnly);
	if (bRelayOnly)
	{
		OutResult = FWebRTC4UnrealOperationResult::Failure(TEXT("backend_relay_only_rejected"),
			TEXT("The backend requested forced TURN, but this client uses direct-first ICE"));
		return false;
	}

	const TSharedRef<FFlowWebRTCTransportContext> Context =
		MakeShared<FFlowWebRTCTransportContext>();
	Context->RoomId = GetString(Json, TEXT("room_id"));
	Context->Protocol = GetString(Json, TEXT("protocol"));
	Context->LocalPrincipalId = GetString(Json, TEXT("local_principal_id"));
	Context->HostPrincipalId = GetString(Json, TEXT("host_principal_id"));
	Context->IceExpiresAt = GetString(Json, TEXT("ice_expires_at"));
	Json->TryGetNumberField(TEXT("max_participants"), Context->MaxParticipants);
	ReadStringArray(Json, TEXT("signaling_urls"), Context->SignallingUrls);
	ReadStringArray(Json, TEXT("asyncapi_urls"), Context->AsyncApiUrls);
	if (Context->SignallingUrls.IsEmpty())
	{
		const FString SignallingUrl = GetString(Json, TEXT("signal_url"));
		if (!SignallingUrl.IsEmpty()) Context->SignallingUrls.Add(SignallingUrl);
	}
	if (Json->HasTypedField<EJson::Object>(TEXT("signaling_auth")))
	{
		const TSharedPtr<FJsonObject> Authentication = Json->GetObjectField(TEXT("signaling_auth"));
		Context->PrincipalContext = GetString(Authentication, TEXT("principal_context"));
		Context->Timestamp = GetString(Authentication, TEXT("timestamp"));
		Context->Signature = GetString(Authentication, TEXT("signature"));
	}

	bool bHasStun = false;
	bool bHasAuthenticatedTurn = false;
	const TArray<TSharedPtr<FJsonValue>>* IceServers = nullptr;
	if (Json->TryGetArrayField(TEXT("ice_servers"), IceServers) && IceServers)
	{
		for (const TSharedPtr<FJsonValue>& Value : *IceServers)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value || !Value->TryGetObject(Object) || !Object || !*Object) continue;
			FFlowWebRTCIceServer Server;
			ReadStringArray(*Object, TEXT("urls"), Server.Urls);
			Server.Username = GetString(*Object, TEXT("username"));
			Server.Credential = GetString(*Object, TEXT("credential"));
			for (const FString& Url : Server.Urls)
			{
				bHasStun |= Url.StartsWith(TEXT("stun:"), ESearchCase::IgnoreCase)
					|| Url.StartsWith(TEXT("stuns:"), ESearchCase::IgnoreCase);
				if (Url.StartsWith(TEXT("turn:"), ESearchCase::IgnoreCase)
					|| Url.StartsWith(TEXT("turns:"), ESearchCase::IgnoreCase))
				{
					bHasAuthenticatedTurn |= !Server.Username.IsEmpty()
						&& !Server.Credential.IsEmpty();
				}
			}
			if (!Server.Urls.IsEmpty()) Context->IceServers.Add(MoveTemp(Server));
		}
	}

	if (Context->RoomId.IsEmpty() || Context->Protocol != TEXT("flow-signaling.v1")
		|| Context->SignallingUrls.IsEmpty() || Context->PrincipalContext.IsEmpty()
		|| Context->Timestamp.IsEmpty() || Context->Signature.IsEmpty()
		|| Context->LocalPrincipalId.IsEmpty() || Context->HostPrincipalId.IsEmpty()
		|| !bHasStun || !bHasAuthenticatedTurn)
	{
		OutResult = FWebRTC4UnrealOperationResult::Failure(TEXT("backend_invalid_session"),
			TEXT("Backend response is missing Flow signalling, peer identity, STUN, or TURN data"));
		return false;
	}
	if (bHost && Context->LocalPrincipalId != Context->HostPrincipalId)
	{
		OutResult = FWebRTC4UnrealOperationResult::Failure(TEXT("backend_invalid_host_identity"),
			TEXT("Flow host session returned mismatched local and host principals"));
		return false;
	}

	const FString ContextKey = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	if (!FWebRTC4UnrealTransportRegistry::RegisterContext(
		ContextKey, FlowTransportName, Context, bHost))
	{
		OutResult = FWebRTC4UnrealOperationResult::Failure(TEXT("transport_registration_failed"),
			TEXT("Could not register the Flow transport context"));
		return false;
	}
	ActiveContextKeys.Add(ContextKey);

	FWebRTC4UnrealSessionDescriptor Session;
	Session.Provider = TEXT("Flow");
	Session.RoomId = Context->RoomId;
	Session.RoomName = GetString(Json, TEXT("room_name"));
	Session.TransportName = FlowTransportName;
	Session.TransportContextKey = ContextKey;
	Session.MaxParticipants = Context->MaxParticipants;
	Session.bRelayOnly = false;
	Session.ConnectString = FString::Printf(
		TEXT("webrtc4unreal.invalid?WebRTC4UnrealTransport=%s?WebRTC4UnrealContext=%s"),
		*FlowTransportName.ToString(), *ContextKey);
	UE_LOG(LogFlowWebRTCProvider, Display,
		TEXT("WEBRTC4UNREAL_FLOW_SESSION role=%s room=%s max=%d ice_servers=%d"),
		bHost ? TEXT("host") : TEXT("client"), *Session.RoomId,
		Session.MaxParticipants, Context->IceServers.Num());
	OutResult = FWebRTC4UnrealOperationResult::Success(Session,
		TEXT("Flow multi-peer WebRTC session is ready"));
	return true;
}

bool FFlowWebRTCProvider::ParseObjectResponse(
	const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
	bool bTransportSucceeded, const TArray<int32>& ExpectedCodes,
	TSharedPtr<FJsonObject>& OutObject, FWebRTC4UnrealOperationResult& OutError)
{
	if (!bTransportSucceeded || !Response || !ExpectedCodes.Contains(Response->GetResponseCode()))
	{
		OutError = MakeHttpError(Response, bTransportSucceeded);
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject)
	{
		OutError = FWebRTC4UnrealOperationResult::Failure(TEXT("backend_invalid_json"),
			FString::Printf(TEXT("Backend returned invalid JSON (HTTP %d)"),
				Response->GetResponseCode()));
		return false;
	}
	return true;
}

FWebRTC4UnrealOperationResult FFlowWebRTCProvider::MakeHttpError(
	const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
	bool bTransportSucceeded)
{
	if (!bTransportSucceeded || !Response)
	{
		return FWebRTC4UnrealOperationResult::Failure(TEXT("backend_network_error"),
			TEXT("Trusted Flow backend could not be reached"));
	}
	FString Code = FString::Printf(TEXT("backend_http_%d"), Response->GetResponseCode());
	FString Message = FString::Printf(TEXT("Trusted Flow backend failed with HTTP %d"),
		Response->GetResponseCode());
	TSharedPtr<FJsonObject> Envelope;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (FJsonSerializer::Deserialize(Reader, Envelope) && Envelope)
	{
		if (Envelope->HasTypedField<EJson::Object>(TEXT("error")))
		{
			const TSharedPtr<FJsonObject> Error = Envelope->GetObjectField(TEXT("error"));
			const FString ApiCode = GetString(Error, TEXT("code"));
			const FString ApiMessage = GetString(Error, TEXT("message"));
			if (!ApiCode.IsEmpty()) Code = ApiCode;
			if (!ApiMessage.IsEmpty()) Message = ApiMessage;
		}
		else
		{
			const FString ApiCode = GetString(Envelope, TEXT("error"));
			const FString ApiMessage = GetString(Envelope, TEXT("message"));
			if (!ApiCode.IsEmpty()) Code = ApiCode;
			if (!ApiMessage.IsEmpty()) Message = ApiMessage;
		}
	}
	return FWebRTC4UnrealOperationResult::Failure(Code, Message);
}

void FFlowWebRTCProvider::ReadStringArray(const TSharedPtr<FJsonObject>& Object,
	const FString& FieldName, TArray<FString>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object || !Object->TryGetArrayField(FieldName, Values) || !Values) return;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString StringValue;
		if (Value && Value->TryGetString(StringValue)) OutValues.Add(MoveTemp(StringValue));
	}
}
