// McpLinkControlRig: Control Rig interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkControlRig : ModuleRules
{
	public McpLinkControlRig(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp23;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"McpLinkCore",
			"UnrealEd",
			"Kismet",
			"AssetRegistry",
			"MovieScene",
			"MovieSceneTracks",
			"LevelSequence",
			"RigVM",
			"RigVMDeveloper",
			"ControlRig",
			"ControlRigDeveloper",
			"ControlRigEditor"
		});
	}
}
