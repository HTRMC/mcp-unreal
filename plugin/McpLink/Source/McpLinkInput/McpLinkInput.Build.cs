// McpLinkInput: stateful PIE input injection (keys, mouse, axes, Enhanced Input actions).

using UnrealBuildTool;

public class McpLinkInput : ModuleRules
{
	public McpLinkInput(ReadOnlyTargetRules Target) : base(Target)
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
			"InputCore",
			"ApplicationCore",
			"EnhancedInput"
		});
	}
}
