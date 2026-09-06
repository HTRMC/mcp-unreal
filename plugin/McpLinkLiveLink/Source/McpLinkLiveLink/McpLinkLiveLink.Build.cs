// McpLinkLiveLink: Live Link interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkLiveLink : ModuleRules
{
	public McpLinkLiveLink(ReadOnlyTargetRules Target) : base(Target)
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
			"LiveLinkInterface",
			"LiveLink"
		});
	}
}
