// McpLinkWorld: world-building routes — landscape terrain, foliage scatter,
// sublevels, World Partition data layers, Level Instances and actor merging.

using UnrealBuildTool;

public class McpLinkWorld : ModuleRules
{
	public McpLinkWorld(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp23;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",
			"McpLinkCore",
			"UnrealEd",
			"AssetRegistry",
			"AssetTools",
			"Landscape",
			"Foliage",
			"DataLayerEditor",
			"MeshMergeUtilities",
			"MeshUtilities"
		});
	}
}
