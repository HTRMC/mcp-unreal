// McpLinkToolsets: Toolset Registry interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkToolsets : ModuleRules
{
	public McpLinkToolsets(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp23;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"DeveloperSettings",
			"EditorSubsystem",
			"Json",
			"McpLinkCore",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
