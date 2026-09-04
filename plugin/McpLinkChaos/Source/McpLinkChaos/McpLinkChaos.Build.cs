// McpLinkChaos: Chaos destruction interop routes for McpLink — Geometry
// Collection assets and the fracture operations the Fracture editor mode runs.

using UnrealBuildTool;

public class McpLinkChaos : ModuleRules
{
	public McpLinkChaos(ReadOnlyTargetRules Target) : base(Target)
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
			"GeometryCollectionEngine",
			"GeometryCollectionEditor",
			"FractureEngine",
			"DataflowCore",
			"Chaos"
		});
	}
}
