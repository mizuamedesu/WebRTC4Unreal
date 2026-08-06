#pragma once

#include "CoreMinimal.h"
#include "WebRTC4UnrealTypes.generated.h"

UENUM(BlueprintType)
enum class EWebRTC4UnrealConnectionState : uint8
{
	Idle,
	Resolving,
	Hosting,
	Joining,
	Traveling,
	Connected,
	Failed
};

USTRUCT(BlueprintType)
struct WEBRTC4UNREALCORE_API FWebRTC4UnrealProviderConfiguration
{
	GENERATED_BODY()

	/** Provider endpoint, such as a trusted session broker URL. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRTC4Unreal")
	FString Endpoint;

	/** Opaque provider options. Core never interprets or logs their values. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRTC4Unreal")
	TMap<FString, FString> Options;
};

USTRUCT(BlueprintType)
struct WEBRTC4UNREALCORE_API FWebRTC4UnrealHostRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRTC4Unreal")
	FString RoomName = TEXT("Unreal WebRTC Room");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRTC4Unreal")
	FString MapPath = TEXT("/Game/FirstPerson/Lvl_FirstPerson");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRTC4Unreal", meta = (ClampMin = "1", ClampMax = "65535"))
	int32 ListenPort = 7777;

	/** Address placed in provider metadata, for example 127.0.0.1:7777 or a public host:port. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRTC4Unreal")
	FString AdvertisedAddress = TEXT("127.0.0.1:7777");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRTC4Unreal", meta = (ClampMin = "2", ClampMax = "1000"))
	int32 MaxParticipants = 4;
};

USTRUCT(BlueprintType)
struct WEBRTC4UNREALCORE_API FWebRTC4UnrealJoinRequest
{
	GENERATED_BODY()

	/** Provider room UUID/name, or a host:port for DirectIp. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRTC4Unreal")
	FString RoomReference;
};

USTRUCT(BlueprintType)
struct WEBRTC4UNREALCORE_API FWebRTC4UnrealSessionDescriptor
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	FName Provider;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	FString RoomId;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	FString RoomName;

	/** Unreal client travel address resolved by the provider. */
	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	FString ConnectString;

	/** Registered data-plane implementation. None means normal IP networking. */
	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	FName TransportName;

	/** Opaque in-memory context key consumed by the registered transport. */
	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	FString TransportContextKey;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	int32 MaxParticipants = 0;

	/** True only when the provider explicitly restricts its transport to a relay. */
	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	bool bRelayOnly = false;
};

/** Provider-independent live network information for UI and Blueprints. */
USTRUCT(BlueprintType)
struct WEBRTC4UNREALCORE_API FWebRTC4UnrealNetworkStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal|Network")
	EWebRTC4UnrealConnectionState State = EWebRTC4UnrealConnectionState::Idle;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal|Network")
	bool bConnected = false;

	/** Unreal packet acknowledgement round-trip time. Negative until measurable. */
	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal|Network")
	float PingMilliseconds = -1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal|Network")
	FName Provider;

	/** Active data-plane transport, or None for the native IP path. */
	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal|Network")
	FName TransportName;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal|Network")
	bool bRelayOnly = false;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal|Network")
	bool bIsHost = false;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal|Network")
	int32 ConnectedPeerCount = 0;
};

USTRUCT(BlueprintType)
struct WEBRTC4UNREALCORE_API FWebRTC4UnrealOperationResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	FString ErrorCode;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category = "WebRTC4Unreal")
	FWebRTC4UnrealSessionDescriptor Session;

	static FWebRTC4UnrealOperationResult Success(const FWebRTC4UnrealSessionDescriptor& InSession, const FString& InMessage = FString())
	{
		FWebRTC4UnrealOperationResult Result;
		Result.bSuccess = true;
		Result.Message = InMessage;
		Result.Session = InSession;
		return Result;
	}

	static FWebRTC4UnrealOperationResult Failure(const FString& InCode, const FString& InMessage)
	{
		FWebRTC4UnrealOperationResult Result;
		Result.ErrorCode = InCode;
		Result.Message = InMessage;
		return Result;
	}
};
