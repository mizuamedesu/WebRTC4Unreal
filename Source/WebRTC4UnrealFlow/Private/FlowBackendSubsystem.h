#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FlowBackendSubsystem.generated.h"

/**
 * Development-only launcher for the bundled Flow credential broker.
 * This lives in the Flow adapter so the provider-agnostic Core never owns or
 * even sees a long-lived Flow credential.
 */
UCLASS()
class UFlowBackendSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void StartBundledBackend();
	void ReleaseBundledBackendHandle();

	bool bAutoStartBundledBackend = false;
	int32 BundledBackendPort = 64208;
	FString HostPrincipalId;
	uint32 BundledBackendProcessId = 0;
	FProcHandle BundledBackendProcess;
};
