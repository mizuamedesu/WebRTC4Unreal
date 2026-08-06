using UnrealBuildTool;

public class WebRTC4UnrealCore : ModuleRules
{
	public WebRTC4UnrealCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"OnlineSubsystemUtils",
			"UMG"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"ApplicationCore",
			"NetCore",
			"PacketHandler",
			"Projects",
			"Slate",
			"SlateCore",
			"Sockets"
		});
	}
}
