// McpLinkLevelSnapshots: Level Snapshots interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkLevelSnapshots : ModuleRules
{
	public McpLinkLevelSnapshots(ReadOnlyTargetRules Target) : base(Target)
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
			"LevelSnapshots",
			"LevelSnapshotFilters"
		});
	}
}
