using UnrealBuildTool;

public class WebRTC4UnrealCloudflareRealtime : ModuleRules
{
	public WebRTC4UnrealCloudflareRealtime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"WebRTC4UnrealCore"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Engine",
			"HTTP",
			"Json",
			"PixelStreaming",
			"WebRTC",
			"WebSockets"
		});
	}
}
