// McpLinkMovieRender: Movie Render Queue routes for McpLink.

using UnrealBuildTool;

public class McpLinkMovieRender : ModuleRules
{
	public McpLinkMovieRender(ReadOnlyTargetRules Target) : base(Target)
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
			"LevelSequence",
			"MovieScene",
			"MovieRenderPipelineCore",
			"MovieRenderPipelineEditor",
			"MovieRenderPipelineRenderPasses",
			"MovieRenderPipelineSettings"
		});
	}
}
