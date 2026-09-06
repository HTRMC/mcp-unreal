// McpLinkMass: Mass Entity interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkMass : ModuleRules
{
	public McpLinkMass(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp23;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",
			"McpLinkCore",
			"UnrealEd",
			"AssetRegistry",
			"StructUtils",
			"MassEntity",
			"MassSpawner"
		});
	}
}
