#pragma once

#include "Blueprint/UserWidget.h"
#include "WebRTC4UnrealTypes.h"
#include "WebRTC4UnrealLobbyWidget.generated.h"

class SButton;
class SEditableTextBox;
class STextBlock;

/** Small runtime lobby UI. It is built entirely in C++ so no project widget asset is required. */
UCLASS(BlueprintType)
class WEBRTC4UNREALCORE_API UWebRTC4UnrealLobbyWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetDefaults(FName InProviderName, const FString& InProviderEndpoint,
		const FString& InProviderAccessKey, const FString& InAdvertisedAddress,
		int32 InListenPort, int32 InMaxParticipants);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

private:
	enum class EPendingLobbyAction : uint8
	{
		None,
		Host,
		Join
	};

	FReply HandleCreateClicked();
	FReply HandleJoinClicked();
	FReply HandleCopyRoomClicked();
	FReply HandleCloseClicked();

	UFUNCTION()
	void HandleP2PStateChanged(EWebRTC4UnrealConnectionState NewState, const FString& Detail);

	UFUNCTION()
	void HandleP2POperationCompleted(const FWebRTC4UnrealOperationResult& Result);

	class UWebRTC4UnrealSubsystem* GetWebRTC4UnrealSubsystem() const;
	void SetStatus(const FText& Text, bool bError = false);
	void SetActionsEnabled(bool bEnabled);
	FName DefaultProviderName;
	FString DefaultProviderEndpoint;
	FString DefaultProviderAccessKey;
	FString DefaultAdvertisedAddress;
	int32 DefaultListenPort = 7777;
	int32 DefaultMaxParticipants = 4;
	EPendingLobbyAction PendingAction = EPendingLobbyAction::None;

	TSharedPtr<SEditableTextBox> RoomReferenceInput;
	TSharedPtr<SEditableTextBox> RoomIdOutput;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SButton> CreateButton;
	TSharedPtr<SButton> JoinButton;
};
