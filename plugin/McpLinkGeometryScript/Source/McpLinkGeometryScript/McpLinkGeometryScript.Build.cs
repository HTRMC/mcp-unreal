// McpLinkGeometryScript: Geometry Script interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkGeometryScript : ModuleRules
{
	public McpLinkGeometryScript(ReadOnlyTargetRules Target) : base(Target)
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
			"GeometryCore",
			"GeometryFramework",
			"GeometryScriptingCore",
			"GeometryScriptingEditor"
		});
	}
}
