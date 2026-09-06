// McpLinkCore: HTTP server lifecycle, route registry, responder, log capture, status.

using UnrealBuildTool;

public class McpLinkCore : ModuleRules
{
	public McpLinkCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp23;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"HTTPServer"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"AssetTools",
			"AssetRegistry"
		});
	}
}
