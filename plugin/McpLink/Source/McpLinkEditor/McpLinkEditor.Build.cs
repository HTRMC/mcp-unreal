// McpLinkEditor: reflection, actors, levels, asset discovery and lifecycle,
// console, PIE and capture routes.

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
			"AutomationDriver",
			"InputCore",
			"UnrealEd",
			"AssetRegistry",
			"AssetTools",
			"ImageCore",
			"ImageWrapper",
			"RenderCore",
			"RHI",
			"Slate",
			"SlateCore",
			"UMG",
			"EditorSubsystem",
			"InterchangeCore",
			"InterchangeEngine"
		});
	}
}
