// McpLinkMetaSound: MetaSound authoring routes for McpLink.

using UnrealBuildTool;

public class McpLinkMetaSound : ModuleRules
{
	public McpLinkMetaSound(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp23;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"McpLinkCore",
			"UnrealEd",
			"AssetRegistry",
			"AssetTools",
			"MetasoundEngine",
			"MetasoundFrontend",
			"MetasoundEditor",
			"AudioExtensions"
		});
	}
}
