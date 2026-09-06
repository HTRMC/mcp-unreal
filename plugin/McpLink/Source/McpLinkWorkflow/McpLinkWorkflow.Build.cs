// McpLinkWorkflow: the editor's own housekeeping — source control, data
// validation and Map Check, gameplay tags, curve assets, localization targets,
// and the reference graph behind the Reference Viewer and Size Map.
//
// DataValidation and GameplayTagsEditor are engine plugins rather than engine
// modules; McpLink.uplugin marks both as required so the dependency is honest.

using UnrealBuildTool;

public class McpLinkWorkflow : ModuleRules
{
	public McpLinkWorkflow(ReadOnlyTargetRules Target) : base(Target)
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
			"SourceControl",
			"DataValidation",
			"GameplayTags",
			"GameplayTagsEditor",
			"Kismet",
			"MessageLog",
			"Localization",
			"CollectionManager",
			"Blutility",
			"UMG",
			"UMGEditor",
			"DerivedDataCache"
		});

		// Live Coding only exists on Windows; the route reports it absent elsewhere.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
