#include "CloudflareRealtimeProvider.h"

#include "CloudflareRealtimeTransport.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WebRTC4UnrealTransportRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogCloudflareRealtimeProvider, Log, All);

namespace
{
	const FName CloudflareRealtimeTransportName(TEXT("CloudflareRealtime"));

	FString GetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		FString Value;
		if (Object) Object->TryGetStringField(FieldName, Value);
		return Value;
	}
}

void FCloudflareRealtimeProvider::Configure(
	const FWebRTC4UnrealProviderConfiguration& InConfiguration)
{
	Configuration = InConfiguration;
	Configuration.Endpoint = Configuration.Endpoint.TrimStartAndEnd();
	while (Configuration.Endpoint.EndsWith(TEXT("/")))
	{
		Configuration.Endpoint.LeftChopInline(1, EAllowShrinking::No);
	}
}

bool FCloudflareRealtimeProvider::ValidateConfiguration(
	FWebRTC4UnrealProviderCompletion& Completion) const
{
	if (Configuration.Endpoint.IsEmpty())
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("cloudflare_endpoint_missing"),
			TEXT("Cloudflare Realtime Worker URL is empty")));
		return false;
	}
	const bool bLocalEndpoint = Configuration.Endpoint.StartsWith(TEXT("http://127.0.0.1"))
		|| Configuration.Endpoint.StartsWith(TEXT("http://localhost"));
	if (!Configuration.Endpoint.StartsWith(TEXT("https://")) && !bLocalEndpoint)
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("cloudflare_endpoint_insecure"),
			TEXT("Cloudflare Realtime Worker must use HTTPS outside local development")));
		return false;
	}
	if (Configuration.Options.FindRef(TEXT("AccessKey")).IsEmpty())
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("cloudflare_access_key_missing"),
			TEXT("Cloudflare Realtime client bootstrap key is empty")));
		return false;
	}
	return true;
}

void FCloudflareRealtimeProvider::Host(const FWebRTC4UnrealHostRequest& Request,
	FWebRTC4UnrealProviderCompletion Completion)
{
	if (!ValidateConfiguration(Completion)) return;
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("room_name"), Request.RoomName);
	Body->SetNumberField(TEXT("max_participants"), FMath::Clamp(Request.MaxParticipants, 2, 100));
	SendWorkerRequest(TEXT("/v1/rooms"), TEXT("POST"), Body,
		[this, Completion = MoveTemp(Completion)](
			TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded) mutable
		{
			FWebRTC4UnrealOperationResult Result;
			ParseAndRegisterSession(Response, bSucceeded, true, Result);
			Completion(MoveTemp(Result));
		});
}

void FCloudflareRealtimeProvider::Join(const FWebRTC4UnrealJoinRequest& Request,
	FWebRTC4UnrealProviderCompletion Completion)
{
	if (!ValidateConfiguration(Completion)) return;
	const FString RoomId = Request.RoomReference.TrimStartAndEnd();
	if (RoomId.IsEmpty())
	{
		Completion(FWebRTC4UnrealOperationResult::Failure(TEXT("cloudflare_room_missing"),
			TEXT("Paste the room ID shown by the host")));
		return;
	}
	const FString Path = FString::Printf(TEXT("/v1/rooms/%s/join"),
		*FGenericPlatformHttp::UrlEncode(RoomId));
	SendWorkerRequest(Path, TEXT("POST"), MakeShared<FJsonObject>(),
		[this, Completion = MoveTemp(Completion)](
			TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> Response, bool bSucceeded) mutable
		{
			FWebRTC4UnrealOperationResult Result;
			ParseAndRegisterSession(Response, bSucceeded, false, Result);
			Completion(MoveTemp(Result));
		});
}

void FCloudflareRealtimeProvider::Leave()
{
	for (const FString& Key : ActiveContextKeys)
	{
		FWebRTC4UnrealTransportRegistry::RemoveContext(Key);
	}
	ActiveContextKeys.Reset();
}

void FCloudflareRealtimeProvider::SendWorkerRequest(const FString& Path,
	const FString& Verb, const TSharedPtr<FJsonObject>& Body, FHttpCallback Callback)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		FHttpModule::Get().CreateRequest();
	const TSharedRef<FHttpCallback, ESPMode::ThreadSafe> SharedCallback =
		MakeShared<FHttpCallback, ESPMode::ThreadSafe>(MoveTemp(Callback));
	Request->SetURL(Configuration.Endpoint + Path);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Cache-Control"), TEXT("no-store"));
	Request->SetHeader(TEXT("X-P2P-Bootstrap-Key"), Configuration.Options.FindRef(TEXT("AccessKey")));
	if (Body)
	{
		FString SerializedBody;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&SerializedBody);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		Request->SetContentAsString(SerializedBody);
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

bool FCloudflareRealtimeProvider::ParseAndRegisterSession(
	const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
	bool bTransportSucceeded, bool bHost, FWebRTC4UnrealOperationResult& OutResult)
{
	TSharedPtr<FJsonObject> Json;
	FWebRTC4UnrealOperationResult HttpError;
	if (!ParseObjectResponse(Response, bTransportSucceeded, { 201 }, Json, HttpError))
	{
		OutResult = MoveTemp(HttpError);
		return false;
	}

	const TSharedRef<FCloudflareRealtimeTransportContext> Context =
		MakeShared<FCloudflareRealtimeTransportContext>();
	Context->WorkerUrl = GetString(Json, TEXT("worker_url"));
	if (Context->WorkerUrl.IsEmpty()) Context->WorkerUrl = Configuration.Endpoint;
	Context->SignalUrl = GetString(Json, TEXT("signal_url"));
	Context->Protocol = GetString(Json, TEXT("protocol"));
	Context->RoomId = GetString(Json, TEXT("room_id"));
	Context->RoomName = GetString(Json, TEXT("room_name"));
	Context->ParticipantId = GetString(Json, TEXT("participant_id"));
	Context->ParticipantToken = GetString(Json, TEXT("participant_token"));
	Context->HostId = GetString(Json, TEXT("host_id"));
	Json->TryGetNumberField(TEXT("max_participants"), Context->MaxParticipants);

	const FString ExpectedRole = bHost ? TEXT("host") : TEXT("client");
	if (Context->Protocol != TEXT("cloudflare-realtime.v1")
		|| Context->RoomId.IsEmpty() || Context->SignalUrl.IsEmpty()
		|| Context->ParticipantId.IsEmpty() || Context->ParticipantToken.IsEmpty()
		|| Context->HostId.IsEmpty() || GetString(Json, TEXT("role")) != ExpectedRole)
	{
		OutResult = FWebRTC4UnrealOperationResult::Failure(TEXT("cloudflare_invalid_session"),
			TEXT("Worker response is missing Realtime room or participant data"));
		return false;
	}
	if (bHost && Context->ParticipantId != Context->HostId)
	{
		OutResult = FWebRTC4UnrealOperationResult::Failure(TEXT("cloudflare_invalid_host"),
			TEXT("Worker returned a mismatched listen-server identity"));
		return false;
	}

	const FString ContextKey = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	if (!FWebRTC4UnrealTransportRegistry::RegisterContext(
		ContextKey, CloudflareRealtimeTransportName, Context, bHost))
	{
		OutResult = FWebRTC4UnrealOperationResult::Failure(TEXT("transport_registration_failed"),
			TEXT("Could not register the Cloudflare Realtime transport context"));
		return false;
	}
	ActiveContextKeys.Add(ContextKey);

	FWebRTC4UnrealSessionDescriptor Session;
	Session.Provider = TEXT("CloudflareRealtime");
	Session.RoomId = Context->RoomId;
	Session.RoomName = Context->RoomName;
	Session.TransportName = CloudflareRealtimeTransportName;
	Session.TransportContextKey = ContextKey;
	Session.MaxParticipants = Context->MaxParticipants;
	Session.bRelayOnly = true;
	Session.ConnectString = FString::Printf(
		TEXT("webrtc4unreal.invalid?WebRTC4UnrealTransport=%s?WebRTC4UnrealContext=%s"),
		*CloudflareRealtimeTransportName.ToString(), *ContextKey);
	UE_LOG(LogCloudflareRealtimeProvider, Display,
		TEXT("WEBRTC4UNREAL_CLOUDFLARE_SESSION role=%s room=%s participant=%s max=%d route=sfu"),
		bHost ? TEXT("host") : TEXT("client"), *Session.RoomId,
		*Context->ParticipantId, Session.MaxParticipants);
	OutResult = FWebRTC4UnrealOperationResult::Success(Session,
		TEXT("Cloudflare Realtime SFU session is ready"));
	return true;
}

bool FCloudflareRealtimeProvider::ParseObjectResponse(
	const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
	bool bTransportSucceeded, const TArray<int32>& ExpectedCodes,
	TSharedPtr<FJsonObject>& OutObject, FWebRTC4UnrealOperationResult& OutError)
{
	if (!bTransportSucceeded || !Response || !ExpectedCodes.Contains(Response->GetResponseCode()))
	{
		OutError = MakeHttpError(Response, bTransportSucceeded);
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject)
	{
		OutError = FWebRTC4UnrealOperationResult::Failure(TEXT("cloudflare_invalid_json"),
			TEXT("Cloudflare Realtime Worker returned invalid JSON"));
		return false;
	}
	return true;
}

FWebRTC4UnrealOperationResult FCloudflareRealtimeProvider::MakeHttpError(
	const TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>& Response,
	bool bTransportSucceeded)
{
	if (!bTransportSucceeded || !Response)
	{
		return FWebRTC4UnrealOperationResult::Failure(TEXT("cloudflare_network_error"),
			TEXT("Cloudflare Realtime Worker could not be reached"));
	}
	FString Code = FString::Printf(TEXT("cloudflare_http_%d"), Response->GetResponseCode());
	FString Message = FString::Printf(TEXT("Cloudflare Realtime Worker failed with HTTP %d"),
		Response->GetResponseCode());
	TSharedPtr<FJsonObject> Envelope;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (FJsonSerializer::Deserialize(Reader, Envelope) && Envelope
		&& Envelope->HasTypedField<EJson::Object>(TEXT("error")))
	{
		const TSharedPtr<FJsonObject> Error = Envelope->GetObjectField(TEXT("error"));
		const FString ApiCode = GetString(Error, TEXT("code"));
		const FString ApiMessage = GetString(Error, TEXT("message"));
		if (!ApiCode.IsEmpty()) Code = ApiCode;
		if (!ApiMessage.IsEmpty()) Message = ApiMessage;
	}
	return FWebRTC4UnrealOperationResult::Failure(Code, Message);
}
