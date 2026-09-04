// McpLinkNiagara: Niagara interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkNiagara : ModuleRules
{
	public McpLinkNiagara(ReadOnlyTargetRules Target) : base(Target)
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
			"Niagara",
			"NiagaraCore"
		});
	}
}
