// McpLinkGAS: Gameplay Ability System interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkGAS : ModuleRules
{
	public McpLinkGAS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp23;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"McpLinkCore",
			"UnrealEd",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks"
		});
	}
}
