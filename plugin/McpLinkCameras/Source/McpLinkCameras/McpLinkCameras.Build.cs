// McpLinkCameras: Gameplay Cameras interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkCameras : ModuleRules
{
	public McpLinkCameras(ReadOnlyTargetRules Target) : base(Target)
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
			"GameplayCameras"
		});
	}
}
