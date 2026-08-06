#include "WebRTC4UnrealLobbyWidget.h"

#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformApplicationMisc.h"
#include "WebRTC4UnrealLobbySubsystem.h"
#include "WebRTC4UnrealSubsystem.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "WebRTC4UnrealLobbyWidget"

void UWebRTC4UnrealLobbyWidget::SetDefaults(FName InProviderName,
	const FString& InProviderEndpoint, const FString& InProviderAccessKey,
	const FString& InAdvertisedAddress, int32 InListenPort,
	int32 InMaxParticipants)
{
	DefaultProviderName = InProviderName;
	DefaultProviderEndpoint = InProviderEndpoint;
	DefaultProviderAccessKey = InProviderAccessKey;
	DefaultAdvertisedAddress = InAdvertisedAddress;
	DefaultListenPort = InListenPort;
	DefaultMaxParticipants = FMath::Clamp(InMaxParticipants, 2, 1000);
}

TSharedRef<SWidget> UWebRTC4UnrealLobbyWidget::RebuildWidget()
{
	const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 26);
	const FSlateFontInfo SectionFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 16);
	const FSlateFontInfo NormalFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 12);

	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.BorderBackgroundColor(FLinearColor(0.008f, 0.012f, 0.025f, 0.66f))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(20.0f)
		[
			SNew(SBox)
			.WidthOverride(540.0f)
			[
				SNew(SBorder)
				.Padding(FMargin(28.0f, 24.0f))
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor(FLinearColor(0.035f, 0.055f, 0.10f, 0.98f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Title", "WebRTC4Unreal マッチ"))
						.Font(TitleFont)
						.ColorAndOpacity(FLinearColor(0.38f, 0.82f, 1.0f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Center)
					.Padding(0.0f, 3.0f, 0.0f, 22.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Subtitle", "作成側のルームIDを参加側へ送るだけです"))
						.Font(NormalFont)
						.ColorAndOpacity(FLinearColor(0.65f, 0.72f, 0.82f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("CreateLabel", "1. マッチを作成"))
						.Font(SectionFont)
						.ColorAndOpacity(FLinearColor(0.40f, 0.88f, 0.72f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("CreateHelp", "接続に必要な設定はプロバイダーから自動取得します。"))
						.Font(NormalFont)
						.ColorAndOpacity(FLinearColor(0.75f, 0.80f, 0.88f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						SAssignNew(CreateButton, SButton)
						.HAlign(HAlign_Center)
						.ContentPadding(FMargin(18.0f, 9.0f))
						.OnClicked(FOnClicked::CreateUObject(this, &UWebRTC4UnrealLobbyWidget::HandleCreateClicked))
						[
							SNew(STextBlock).Text(LOCTEXT("CreateButton", "マッチを作成")).Font(SectionFont)
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SAssignNew(RoomIdOutput, SEditableTextBox)
							.IsReadOnly(true)
							.HintText(LOCTEXT("RoomOutputHint", "作成後、ここにルームIDを表示します"))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SButton)
							.OnClicked(FOnClicked::CreateUObject(this, &UWebRTC4UnrealLobbyWidget::HandleCopyRoomClicked))
							[
								SNew(STextBlock).Text(LOCTEXT("CopyButton", "コピー"))
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 20.0f, 0.0f, 18.0f)
					[
						SNew(SSeparator).Thickness(1.0f)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("JoinLabel", "2. ルームIDで参加"))
						.Font(SectionFont)
						.ColorAndOpacity(FLinearColor(1.0f, 0.72f, 0.35f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("JoinHelp", "相手から届いたルームIDを貼り付けてください。"))
						.Font(NormalFont)
						.ColorAndOpacity(FLinearColor(0.75f, 0.80f, 0.88f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SAssignNew(RoomReferenceInput, SEditableTextBox)
						.HintText(LOCTEXT("RoomInputHint", "ルームIDを貼り付け"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 14.0f)
					[
						SAssignNew(JoinButton, SButton)
						.HAlign(HAlign_Center)
						.ContentPadding(FMargin(18.0f, 9.0f))
						.OnClicked(FOnClicked::CreateUObject(this, &UWebRTC4UnrealLobbyWidget::HandleJoinClicked))
						[
							SNew(STextBlock).Text(LOCTEXT("JoinButton", "参加")).Font(SectionFont)
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 5.0f)
					[
						SAssignNew(StatusText, STextBlock)
						.Text(LOCTEXT("Ready", "準備完了"))
						.AutoWrapText(true)
						.ColorAndOpacity(FLinearColor(0.70f, 0.86f, 1.0f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Right)
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.OnClicked(FOnClicked::CreateUObject(this, &UWebRTC4UnrealLobbyWidget::HandleCloseClicked))
						[
							SNew(STextBlock).Text(LOCTEXT("CloseButton", "閉じる"))
						]
					]
				]
			]
		];
}

void UWebRTC4UnrealLobbyWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (UWebRTC4UnrealSubsystem* Subsystem = GetWebRTC4UnrealSubsystem())
	{
		Subsystem->OnStateChanged.AddUniqueDynamic(this, &UWebRTC4UnrealLobbyWidget::HandleP2PStateChanged);
		Subsystem->OnOperationCompleted.AddUniqueDynamic(this, &UWebRTC4UnrealLobbyWidget::HandleP2POperationCompleted);
		const FWebRTC4UnrealSessionDescriptor ExistingSession = Subsystem->GetCurrentSession();
		const FString ShareReference = ExistingSession.RoomId.IsEmpty()
			? ExistingSession.ConnectString : ExistingSession.RoomId;
		if (RoomIdOutput.IsValid() && !ShareReference.IsEmpty())
		{
			RoomIdOutput->SetText(FText::FromString(ShareReference));
		}
	}
}

void UWebRTC4UnrealLobbyWidget::NativeDestruct()
{
	if (UWebRTC4UnrealSubsystem* Subsystem = GetWebRTC4UnrealSubsystem())
	{
		Subsystem->OnStateChanged.RemoveDynamic(this, &UWebRTC4UnrealLobbyWidget::HandleP2PStateChanged);
		Subsystem->OnOperationCompleted.RemoveDynamic(this, &UWebRTC4UnrealLobbyWidget::HandleP2POperationCompleted);
	}
	Super::NativeDestruct();
}

FReply UWebRTC4UnrealLobbyWidget::HandleCreateClicked()
{
	UWebRTC4UnrealSubsystem* Subsystem = GetWebRTC4UnrealSubsystem();
	if (!Subsystem || DefaultProviderName.IsNone() || DefaultProviderEndpoint.IsEmpty())
	{
		SetStatus(LOCTEXT("CreateValidation", "バックエンド設定を読み込めませんでした。"), true);
		return FReply::Handled();
	}

	FWebRTC4UnrealHostRequest Request;
	Request.RoomName = FString::Printf(TEXT("WebRTC4Unreal-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
	Request.MapPath = TEXT("/Game/FirstPerson/Lvl_FirstPerson");
	Request.ListenPort = DefaultListenPort;
	Request.AdvertisedAddress = DefaultAdvertisedAddress;
	Request.MaxParticipants = DefaultMaxParticipants;

	FWebRTC4UnrealProviderConfiguration Configuration;
	Configuration.Endpoint = DefaultProviderEndpoint;
	Configuration.Options.Add(TEXT("SessionBroker"), TEXT("true"));
	Configuration.Options.Add(TEXT("AccessKey"), DefaultProviderAccessKey);
	Subsystem->ConfigureProvider(DefaultProviderName, Configuration);

	PendingAction = EPendingLobbyAction::Host;
	SetActionsEnabled(false);
	SetStatus(LOCTEXT("Creating", "設定を自動取得してマッチを作成しています…"));
	Subsystem->HostSession(DefaultProviderName, Request);
	return FReply::Handled();
}

FReply UWebRTC4UnrealLobbyWidget::HandleJoinClicked()
{
	UWebRTC4UnrealSubsystem* Subsystem = GetWebRTC4UnrealSubsystem();
	const FString RoomReference = RoomReferenceInput.IsValid() ? RoomReferenceInput->GetText().ToString().TrimStartAndEnd() : FString();
	if (!Subsystem || DefaultProviderName.IsNone() || DefaultProviderEndpoint.IsEmpty()
		|| RoomReference.IsEmpty())
	{
		SetStatus(LOCTEXT("JoinValidation", "ルームIDを貼り付けてください。"), true);
		return FReply::Handled();
	}

	FWebRTC4UnrealJoinRequest Request;
	Request.RoomReference = RoomReference;
	FWebRTC4UnrealProviderConfiguration Configuration;
	Configuration.Endpoint = DefaultProviderEndpoint;
	Configuration.Options.Add(TEXT("SessionBroker"), TEXT("true"));
	Configuration.Options.Add(TEXT("AccessKey"), DefaultProviderAccessKey);
	Subsystem->ConfigureProvider(DefaultProviderName, Configuration);
	PendingAction = EPendingLobbyAction::Join;
	SetActionsEnabled(false);
	SetStatus(LOCTEXT("Joining", "設定を自動取得して参加しています…"));
	Subsystem->JoinSession(DefaultProviderName, Request);
	return FReply::Handled();
}

FReply UWebRTC4UnrealLobbyWidget::HandleCopyRoomClicked()
{
	const FString RoomId = RoomIdOutput.IsValid() ? RoomIdOutput->GetText().ToString() : FString();
	if (!RoomId.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*RoomId);
		SetStatus(LOCTEXT("Copied", "ルームIDをコピーしました。相手に送ってください。"));
	}
	else
	{
		SetStatus(LOCTEXT("NothingToCopy", "先にマッチを作成してください。"), true);
	}
	return FReply::Handled();
}

FReply UWebRTC4UnrealLobbyWidget::HandleCloseClicked()
{
	SetVisibility(ESlateVisibility::Collapsed);
	if (APlayerController* PlayerController = GetOwningPlayer())
	{
		PlayerController->SetInputMode(FInputModeGameOnly());
		PlayerController->SetShowMouseCursor(false);
	}
	return FReply::Handled();
}

void UWebRTC4UnrealLobbyWidget::HandleP2PStateChanged(EWebRTC4UnrealConnectionState NewState, const FString& Detail)
{
	if (!Detail.IsEmpty())
	{
		SetStatus(FText::FromString(Detail), NewState == EWebRTC4UnrealConnectionState::Failed);
	}
}

void UWebRTC4UnrealLobbyWidget::HandleP2POperationCompleted(const FWebRTC4UnrealOperationResult& Result)
{
	SetActionsEnabled(true);
	if (!Result.bSuccess)
	{
		PendingAction = EPendingLobbyAction::None;
		SetStatus(FText::FromString(Result.Message.IsEmpty() ? Result.ErrorCode : Result.Message), true);
		return;
	}

	if (PendingAction == EPendingLobbyAction::Host)
	{
		const FString ShareReference = Result.Session.RoomId.IsEmpty()
			? Result.Session.ConnectString : Result.Session.RoomId;
		if (RoomIdOutput.IsValid())
		{
			RoomIdOutput->SetText(FText::FromString(ShareReference));
		}
		SetStatus(LOCTEXT("Created", "作成完了。このルームIDだけを相手に送ってください。"));
	}
	else if (PendingAction == EPendingLobbyAction::Join)
	{
		SetStatus(LOCTEXT("JoinTravel", "参加しました。ゲームへ移動します…"));
		if (UGameInstance* GameInstance = GetGameInstance())
		{
			if (UWebRTC4UnrealLobbySubsystem* LobbySubsystem = GameInstance->GetSubsystem<UWebRTC4UnrealLobbySubsystem>())
			{
				LobbySubsystem->HideLobby();
			}
		}
	}
	PendingAction = EPendingLobbyAction::None;
}

UWebRTC4UnrealSubsystem* UWebRTC4UnrealLobbyWidget::GetWebRTC4UnrealSubsystem() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UWebRTC4UnrealSubsystem>() : nullptr;
}

void UWebRTC4UnrealLobbyWidget::SetStatus(const FText& Text, bool bError)
{
	if (StatusText.IsValid())
	{
		StatusText->SetText(Text);
		StatusText->SetColorAndOpacity(bError ? FLinearColor(1.0f, 0.32f, 0.32f) : FLinearColor(0.70f, 0.86f, 1.0f));
	}
}

void UWebRTC4UnrealLobbyWidget::SetActionsEnabled(bool bEnabled)
{
	if (CreateButton.IsValid()) CreateButton->SetEnabled(bEnabled);
	if (JoinButton.IsValid()) JoinButton->SetEnabled(bEnabled);
}

#undef LOCTEXT_NAMESPACE
