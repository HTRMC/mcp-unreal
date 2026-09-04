// McpLinkPython: Python Editor Script Plugin interop routes for McpLink.

using UnrealBuildTool;

public class McpLinkPython : ModuleRules
{
	public McpLinkPython(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp23;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"McpLinkCore",
			"PythonScriptPlugin"
		});
	}
}
