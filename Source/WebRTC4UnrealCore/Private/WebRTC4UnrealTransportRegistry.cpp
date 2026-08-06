#include "WebRTC4UnrealTransportRegistry.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace
{
	FCriticalSection TransportRegistryMutex;
	TMap<FName, FWebRTC4UnrealTransportRegistry::FFactory> TransportFactories;
	TMap<FString, FWebRTC4UnrealTransportRegistration> TransportContexts;
}

void FWebRTC4UnrealTransportRegistry::RegisterFactory(FName TransportName, FFactory Factory)
{
	FScopeLock Lock(&TransportRegistryMutex);
	TransportFactories.Add(TransportName, MoveTemp(Factory));
}

void FWebRTC4UnrealTransportRegistry::UnregisterFactory(FName TransportName)
{
	FScopeLock Lock(&TransportRegistryMutex);
	TransportFactories.Remove(TransportName);
}

TSharedPtr<IWebRTC4UnrealTransportEndpoint> FWebRTC4UnrealTransportRegistry::CreateEndpoint(FName TransportName)
{
	FFactory Factory;
	{
		FScopeLock Lock(&TransportRegistryMutex);
		const FFactory* Found = TransportFactories.Find(TransportName);
		if (!Found)
		{
			return nullptr;
		}
		Factory = *Found;
	}
	return Factory ? TSharedPtr<IWebRTC4UnrealTransportEndpoint>(Factory()) : nullptr;
}

bool FWebRTC4UnrealTransportRegistry::RegisterContext(const FString& Key, FName TransportName,
	const TSharedRef<IWebRTC4UnrealTransportContext>& Context, bool bHost)
{
	if (Key.IsEmpty() || TransportName.IsNone())
	{
		return false;
	}
	FScopeLock Lock(&TransportRegistryMutex);
	if (TransportContexts.Contains(Key))
	{
		return false;
	}
	FWebRTC4UnrealTransportRegistration& Registration = TransportContexts.Add(Key);
	Registration.TransportName = TransportName;
	Registration.Context = Context;
	Registration.bHost = bHost;
	return true;
}

bool FWebRTC4UnrealTransportRegistry::FindContext(const FString& Key,
	FWebRTC4UnrealTransportRegistration& OutRegistration)
{
	FScopeLock Lock(&TransportRegistryMutex);
	if (const FWebRTC4UnrealTransportRegistration* Registration = TransportContexts.Find(Key))
	{
		OutRegistration = *Registration;
		return true;
	}
	return false;
}

void FWebRTC4UnrealTransportRegistry::RemoveContext(const FString& Key)
{
	FScopeLock Lock(&TransportRegistryMutex);
	TransportContexts.Remove(Key);
}
