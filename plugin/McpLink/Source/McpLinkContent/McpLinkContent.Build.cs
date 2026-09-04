// McpLinkContent: asset authoring routes — materials and material functions,
// render targets, textures and thumbnails, data tables, Enhanced Input assets,
// instanced-static-mesh components, Level Sequences and Sound Cues.

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
			"ImageWrapper",
			"RenderCore",
			"RHI",
			"AudioEditor",
			"StaticMeshEditor",
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
