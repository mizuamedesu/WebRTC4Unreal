#include "IWebRTC4UnrealProvider.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace
{
	FCriticalSection RegistryMutex;
	TMap<FName, FWebRTC4UnrealProviderRegistry::FFactory>& GetFactories()
	{
		static TMap<FName, FWebRTC4UnrealProviderRegistry::FFactory> Factories;
		return Factories;
	}
}

void FWebRTC4UnrealProviderRegistry::RegisterFactory(FName ProviderName, FFactory Factory)
{
	FScopeLock Lock(&RegistryMutex);
	GetFactories().Add(ProviderName, MoveTemp(Factory));
}

void FWebRTC4UnrealProviderRegistry::UnregisterFactory(FName ProviderName)
{
	FScopeLock Lock(&RegistryMutex);
	GetFactories().Remove(ProviderName);
}

TSharedPtr<IWebRTC4UnrealProvider> FWebRTC4UnrealProviderRegistry::Create(FName ProviderName, UWebRTC4UnrealSubsystem& Owner)
{
	FScopeLock Lock(&RegistryMutex);
	const FFactory* Factory = GetFactories().Find(ProviderName);
	return Factory ? TSharedPtr<IWebRTC4UnrealProvider>((*Factory)(Owner)) : nullptr;
}

TArray<FName> FWebRTC4UnrealProviderRegistry::GetProviderNames()
{
	FScopeLock Lock(&RegistryMutex);
	TArray<FName> Result;
	GetFactories().GetKeys(Result);
	Result.Sort(FNameLexicalLess());
	return Result;
}
