// McpLinkRemoteControl: Remote Control preset interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkRemoteControl : ModuleRules
{
	public McpLinkRemoteControl(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp23;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"McpLinkCore",
			"UnrealEd",
			"AssetRegistry",
			"RemoteControl"
		});
	}
}
