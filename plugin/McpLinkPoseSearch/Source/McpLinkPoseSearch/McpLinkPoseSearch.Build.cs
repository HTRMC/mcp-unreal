// McpLinkPoseSearch: Motion Matching (Pose Search) interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkPoseSearch : ModuleRules
{
	public McpLinkPoseSearch(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp23;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",
			"McpLinkCore",
			"UnrealEd",
			"AssetRegistry",
			"PoseSearch"
		});
	}
}
