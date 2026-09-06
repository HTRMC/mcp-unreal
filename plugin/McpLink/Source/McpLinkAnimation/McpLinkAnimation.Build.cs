// McpLinkAnimation: animation asset, skeleton and physics-asset authoring.
//
// Montages, blend spaces and composites are plain UObjects with public
// properties; notifies, curves and virtual bones go through the engine's own
// UAnimationBlueprintLibrary; skeletal-mesh LODs and physics assets go through
// USkeletalMeshEditorSubsystem and FPhysicsAssetUtils.

using UnrealBuildTool;

public class McpLinkAnimation : ModuleRules
{
	public McpLinkAnimation(ReadOnlyTargetRules Target) : base(Target)
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
			"AnimationBlueprintLibrary",
			"SkeletalMeshEditor",
			"PhysicsUtilities",
			"PhysicsCore",
			"ClothingSystemEditor",
			"ClothingSystemEditorInterface",
			"ClothingSystemRuntimeCommon",
			"ClothingSystemRuntimeInterface",
			"MeshDescription",
			"SkeletalMeshDescription",
			"AnimationModifiers",
			"AnimationCore"
		});
	}
}
