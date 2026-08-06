using System;
using System.IO;
using UnrealBuildTool;

public class WebRTC4UnrealFlow : ModuleRules
{
	public WebRTC4UnrealFlow(ReadOnlyTargetRules Target) : base(Target)
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
			"Projects",
			"Sockets",
			"WebRTC",
			"WebSockets"
		});

		string EmbeddedCredential = Environment.GetEnvironmentVariable(
			"WEBRTC4UNREAL_EMBEDDED_DEVELOPER_CREDENTIAL") ?? "";
		if (!String.IsNullOrEmpty(EmbeddedCredential))
		{
			if (Target.Configuration == UnrealTargetConfiguration.Shipping)
			{
				throw new BuildException(
					"Embedding an HCF developer credential is forbidden in Shipping builds.");
			}
			if (!EmbeddedCredential.StartsWith("hcf_", StringComparison.Ordinal))
			{
				throw new BuildException(
					"WEBRTC4UNREAL_EMBEDDED_DEVELOPER_CREDENTIAL is not an HCF credential.");
			}
			foreach (char Character in EmbeddedCredential)
			{
				if (!Char.IsLetterOrDigit(Character) && Character != '_' && Character != '-')
				{
					throw new BuildException("Embedded developer credential contains an unsupported character.");
				}
			}
			PrivateDefinitions.Add("WEBRTC4UNREAL_WITH_EMBEDDED_DEVELOPER_CREDENTIAL=1");
			PrivateDefinitions.Add(
				$"WEBRTC4UNREAL_EMBEDDED_DEVELOPER_CREDENTIAL=\"{EmbeddedCredential}\"");
		}
		else
		{
			PrivateDefinitions.Add("WEBRTC4UNREAL_WITH_EMBEDDED_DEVELOPER_CREDENTIAL=0");
		}

		if (Target.Platform == UnrealTargetPlatform.Win64 && Target.Type == TargetType.Game
			&& !String.IsNullOrEmpty(EmbeddedCredential))
		{
			string NodePath = Environment.GetEnvironmentVariable(
				"WEBRTC4UNREAL_BUNDLED_NODE_PATH") ?? "";
			if (String.IsNullOrEmpty(NodePath))
			{
				NodePath = Path.Combine(Environment.GetFolderPath(
					Environment.SpecialFolder.ProgramFiles), "nodejs", "node.exe");
			}
			string PluginDirectory = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
			string BackendScript = Path.Combine(PluginDirectory, "Resources", "Backend", "server.js");
			if (!File.Exists(NodePath))
			{
				throw new BuildException($"Bundled Node runtime was not found: {NodePath}");
			}
			if (!File.Exists(BackendScript))
			{
				throw new BuildException($"Bundled Flow backend was not found: {BackendScript}");
			}
			RuntimeDependencies.Add(
				Path.Combine("$(TargetOutputDir)", "WebRTC4UnrealBackend", "node.exe"), NodePath);
			RuntimeDependencies.Add(
				Path.Combine("$(TargetOutputDir)", "WebRTC4UnrealBackend", "server.js"), BackendScript);
		}
	}
}
