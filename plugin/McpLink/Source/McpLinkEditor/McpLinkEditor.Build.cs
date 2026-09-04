// McpLinkEditor: reflection, actors, levels, assets, console routes.

using UnrealBuildTool;

public class McpLinkEditor : ModuleRules
{
	public McpLinkEditor(ReadOnlyTargetRules Target) : base(Target)
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
			"ImageCore",
			"ImageWrapper",
			"Slate",
			"SlateCore",
			"UMG",
			"EditorSubsystem"
		});
	}
}
