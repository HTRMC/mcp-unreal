// McpLinkBlueprint: Blueprint asset and graph editing routes.

using UnrealBuildTool;

public class McpLinkBlueprint : ModuleRules
{
	public McpLinkBlueprint(ReadOnlyTargetRules Target) : base(Target)
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
			"McpLinkCore",
			"UnrealEd",
			"AssetRegistry",
			"AssetTools",
			"BlueprintGraph",
			"BlueprintEditorLibrary",
			"Kismet",
			"KismetCompiler",
			"AnimGraph",
			"AnimGraphRuntime",
			"UMG",
			"UMGEditor",
			"JsonUtilities",
			"Slate",
			"SlateCore"
		});
	}
}
