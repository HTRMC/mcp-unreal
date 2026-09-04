// McpLinkAI: Blackboard and Behavior Tree authoring routes.
//
// AIModule and BehaviorTreeEditor ship with the engine rather than as a
// plugin, so this is a module of McpLink itself, not a sibling interop plugin.

using UnrealBuildTool;

public class McpLinkAI : ModuleRules
{
	public McpLinkAI(ReadOnlyTargetRules Target) : base(Target)
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
			"AIModule",
			"AIGraph",
			"BehaviorTreeEditor"
		});
	}
}
