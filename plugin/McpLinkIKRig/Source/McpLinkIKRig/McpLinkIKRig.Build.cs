// McpLinkIKRig: IK Rig and IK Retargeter interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkIKRig : ModuleRules
{
	public McpLinkIKRig(ReadOnlyTargetRules Target) : base(Target)
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
			"IKRig",
			"IKRigEditor",
			"Slate",
			"SlateCore"
		});
	}
}
