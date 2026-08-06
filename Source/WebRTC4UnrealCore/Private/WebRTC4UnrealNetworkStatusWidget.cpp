#include "WebRTC4UnrealNetworkStatusWidget.h"

#include "Engine/GameInstance.h"
#include "WebRTC4UnrealSubsystem.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogWebRTC4UnrealNetworkStatusUI, Log, All);

#define LOCTEXT_NAMESPACE "WebRTC4UnrealNetworkStatusWidget"

TSharedRef<SWidget> UWebRTC4UnrealNetworkStatusWidget::RebuildWidget()
{
	const FSlateFontInfo StatusFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 14);
	const FSlateFontInfo MetricsFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12);
	const FSlateFontInfo TransportFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10);

	return SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(FMargin(16.0f, 14.0f))
		[
			SNew(SBox)
			.MinDesiredWidth(230.0f)
			[
				SNew(SBorder)
				.Padding(FMargin(13.0f, 9.0f))
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor(FLinearColor(0.015f, 0.025f, 0.045f, 0.88f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SAssignNew(ConnectionText, STextBlock)
						.Text(LOCTEXT("Offline", "● オフライン"))
						.Font(StatusFont)
						.ColorAndOpacity(FLinearColor(0.60f, 0.65f, 0.72f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SAssignNew(MetricsText, STextBlock)
						.Text(LOCTEXT("NoPing", "Ping: --  |  状態: 待機中"))
						.Font(MetricsFont)
						.ColorAndOpacity(FLinearColor(0.82f, 0.86f, 0.92f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SAssignNew(TransportText, STextBlock)
						.Text(LOCTEXT("NoTransport", "ネットワーク未接続"))
						.Font(TransportFont)
						.ColorAndOpacity(FLinearColor(0.52f, 0.62f, 0.74f))
					]
				]
			]
		];
}

void UWebRTC4UnrealNetworkStatusWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetVisibility(ESlateVisibility::HitTestInvisible);
	LastUpdateSeconds = 0.0;
	Refresh();
}

void UWebRTC4UnrealNetworkStatusWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	Refresh();
}

void UWebRTC4UnrealNetworkStatusWidget::Refresh()
{
	const double Now = FPlatformTime::Seconds();
	if (Now - LastUpdateSeconds < 0.2)
	{
		return;
	}
	LastUpdateSeconds = Now;
	if (const UWebRTC4UnrealSubsystem* Subsystem = GetWebRTC4UnrealSubsystem())
	{
		UpdateDisplay(Subsystem->GetNetworkStatus());
	}
}

UWebRTC4UnrealSubsystem* UWebRTC4UnrealNetworkStatusWidget::GetWebRTC4UnrealSubsystem() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UWebRTC4UnrealSubsystem>() : nullptr;
}

void UWebRTC4UnrealNetworkStatusWidget::UpdateDisplay(const FWebRTC4UnrealNetworkStatus& Status)
{
	FText ConnectionLabel;
	FText MetricsLabel;
	FLinearColor ConnectionColor;
	FString LogState;

	if (Status.State == EWebRTC4UnrealConnectionState::Failed)
	{
		ConnectionLabel = LOCTEXT("ConnectionError", "● 接続エラー");
		MetricsLabel = LOCTEXT("PingError", "Ping: --  |  状態: 切断");
		ConnectionColor = FLinearColor(1.0f, 0.28f, 0.28f);
		LogState = TEXT("failed");
	}
	else if (Status.bConnected)
	{
		ConnectionLabel = LOCTEXT("Connected", "● 接続済み");
		LogState = TEXT("connected");
		if (Status.PingMilliseconds >= 0.0f)
		{
			const int32 Ping = FMath::RoundToInt(Status.PingMilliseconds);
			const FText Quality = Ping < 80
				? LOCTEXT("QualityGood", "良好")
				: (Ping < 150 ? LOCTEXT("QualityFair", "普通") : LOCTEXT("QualityPoor", "高遅延"));
			MetricsLabel = FText::Format(LOCTEXT("MeasuredPing", "Ping: {0} ms  |  状態: {1}"),
				FText::AsNumber(Ping), Quality);
			ConnectionColor = Ping < 80
				? FLinearColor(0.30f, 0.95f, 0.58f)
				: (Ping < 150 ? FLinearColor(1.0f, 0.78f, 0.28f) : FLinearColor(1.0f, 0.42f, 0.22f));
		}
		else
		{
			MetricsLabel = LOCTEXT("MeasuringPing", "Ping: 計測中…  |  状態: 接続済み");
			ConnectionColor = FLinearColor(0.30f, 0.95f, 0.58f);
		}
	}
	else if (Status.State == EWebRTC4UnrealConnectionState::Resolving
		|| Status.State == EWebRTC4UnrealConnectionState::Joining
		|| Status.State == EWebRTC4UnrealConnectionState::Traveling)
	{
		ConnectionLabel = LOCTEXT("Connecting", "● 接続中");
		MetricsLabel = LOCTEXT("PingConnecting", "Ping: --  |  状態: 接続処理中");
		ConnectionColor = FLinearColor(1.0f, 0.72f, 0.25f);
		LogState = TEXT("connecting");
	}
	else if (Status.State == EWebRTC4UnrealConnectionState::Hosting)
	{
		ConnectionLabel = LOCTEXT("WaitingForPeer", "● 参加待ち");
		MetricsLabel = LOCTEXT("PingWaiting", "Ping: --  |  状態: 接続待機中");
		ConnectionColor = FLinearColor(0.35f, 0.72f, 1.0f);
		LogState = TEXT("waiting");
	}
	else
	{
		ConnectionLabel = LOCTEXT("Idle", "● オフライン");
		MetricsLabel = LOCTEXT("PingIdle", "Ping: --  |  状態: 待機中");
		ConnectionColor = FLinearColor(0.60f, 0.65f, 0.72f);
		LogState = TEXT("idle");
	}

	FText TransportLabel = LOCTEXT("TransportIdle", "ネットワーク未接続");
	if (!Status.TransportName.IsNone())
	{
		const FText BaseLabel = FText::Format(LOCTEXT("ProviderTransport", "{0} / {1}"),
			FText::FromName(Status.Provider), FText::FromName(Status.TransportName));
		TransportLabel = Status.bRelayOnly
			? FText::Format(LOCTEXT("RelayTransport", "{0} (relay-only)"), BaseLabel)
			: BaseLabel;
	}
	else if (!Status.Provider.IsNone())
	{
		TransportLabel = FText::Format(LOCTEXT("NativeTransport", "{0} / Native IP"),
			FText::FromName(Status.Provider));
	}

	if (ConnectionText) ConnectionText->SetText(ConnectionLabel);
	if (ConnectionText) ConnectionText->SetColorAndOpacity(ConnectionColor);
	if (MetricsText) MetricsText->SetText(MetricsLabel);
	if (TransportText) TransportText->SetText(TransportLabel);

	if (LastLoggedState != LogState)
	{
		LastLoggedState = LogState;
		UE_LOG(LogWebRTC4UnrealNetworkStatusUI, Display,
			TEXT("P2P_NETWORK_STATUS state=%s role=%s provider=%s connected=%d"),
			*LogState, Status.bIsHost ? TEXT("host") : TEXT("client"), *Status.Provider.ToString(), Status.bConnected);
	}
	if (Status.bConnected && Status.PingMilliseconds >= 0.0f && !bLoggedFirstPing)
	{
		bLoggedFirstPing = true;
		UE_LOG(LogWebRTC4UnrealNetworkStatusUI, Display, TEXT("P2P_NETWORK_PING role=%s ping_ms=%.1f"),
			Status.bIsHost ? TEXT("host") : TEXT("client"), Status.PingMilliseconds);
	}
}

#undef LOCTEXT_NAMESPACE
