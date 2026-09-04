// McpLinkContent: asset authoring routes — materials, textures, data tables,
// Enhanced Input assets, instanced-static-mesh components and Level Sequences.

using UnrealBuildTool;

public class McpLinkContent : ModuleRules
{
	public McpLinkContent(ReadOnlyTargetRules Target) : base(Target)
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
			"MaterialEditor",
			"EnhancedInput",
			"InputCore",
			"LevelSequence",
			"MovieScene",
			"MovieSceneTracks",
			"UMG",
			"UMGEditor"
		});
	}
}
