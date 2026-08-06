#pragma once

#include "Blueprint/UserWidget.h"
#include "WebRTC4UnrealTypes.h"
#include "WebRTC4UnrealNetworkStatusWidget.generated.h"

class STextBlock;

/** Compact, non-interactive top-left connection and RTT overlay. */
UCLASS(BlueprintType)
class WEBRTC4UNREALCORE_API UWebRTC4UnrealNetworkStatusWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Called by the owning subsystem so native-only widgets update even without a Slate tick source. */
	void Refresh();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	class UWebRTC4UnrealSubsystem* GetWebRTC4UnrealSubsystem() const;
	void UpdateDisplay(const FWebRTC4UnrealNetworkStatus& Status);

	TSharedPtr<STextBlock> ConnectionText;
	TSharedPtr<STextBlock> MetricsText;
	TSharedPtr<STextBlock> TransportText;
	double LastUpdateSeconds = 0.0;
	FString LastLoggedState;
	bool bLoggedFirstPing = false;
};
