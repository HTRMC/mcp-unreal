// The layer stack's index arithmetic, which is the part that is easy to get
// wrong: Blends[i] belongs to Layers[i + 1], so every layer above the
// background has exactly one blend and layer 0 has none.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialLayersFunctions.h"
#include "McpTestFlags.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpMaterialLayerStackTest, "McpLink.Material.LayerStack", McpTestFlags)
bool FMcpMaterialLayerStackTest::RunTest(const FString& Parameters)
{
	UMaterialFunctionMaterialLayer* Rock =
		NewObject<UMaterialFunctionMaterialLayer>(GetTransientPackage());
	UMaterialFunctionMaterialLayer* Moss =
		NewObject<UMaterialFunctionMaterialLayer>(GetTransientPackage());
	UMaterialFunctionMaterialLayerBlend* Blend =
		NewObject<UMaterialFunctionMaterialLayerBlend>(GetTransientPackage());

	FMaterialLayersFunctions Layers;
	TestEqual(TEXT("a fresh stack is empty"), Layers.Layers.Num(), 0);

	// What add_layer does for the first layer: there is nothing to blend against
	// yet, so it becomes the background.
	Layers.AddDefaultBackgroundLayer();
	Layers.Layers[0] = Rock;
	TestEqual(TEXT("background layer added"), Layers.Layers.Num(), 1);
	TestEqual(TEXT("the background layer has no blend"), Layers.Blends.Num(), 0);
	TestEqual(TEXT("the background layer is named"),
		Layers.EditorOnly.LayerNames.Num(), 1);

	// And for every layer above it: one layer, one blend, in step.
	const int32 Index = Layers.AppendBlendedLayer();
	TestEqual(TEXT("the new layer is on top"), Index, 1);
	Layers.Layers[Index] = Moss;
	Layers.Blends[Index - 1] = Blend;
	TestEqual(TEXT("two layers"), Layers.Layers.Num(), 2);
	TestEqual(TEXT("one blend between them"), Layers.Blends.Num(), 1);
	TestTrue(TEXT("the blend belongs to the upper layer"), Layers.Blends[0] == Blend);

	Layers.EditorOnly.LayerNames[1] = FText::FromString(TEXT("Moss"));
	Layers.SetBlendedLayerVisibility(1, false);
	TestFalse(TEXT("the upper layer can be hidden"), Layers.GetLayerVisibility(1));
	TestTrue(TEXT("the background layer stays visible"), Layers.GetLayerVisibility(0));

	// Removing takes the blend with it, so the arrays stay one apart.
	Layers.RemoveBlendedLayerAt(1);
	TestEqual(TEXT("back to one layer"), Layers.Layers.Num(), 1);
	TestEqual(TEXT("its blend went with it"), Layers.Blends.Num(), 0);
	TestEqual(TEXT("and so did its name"), Layers.EditorOnly.LayerNames.Num(), 1);
	TestTrue(TEXT("the background layer is untouched"), Layers.Layers[0] == Rock);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpMaterialLayerMoveTest, "McpLink.Material.LayerMove", McpTestFlags)
bool FMcpMaterialLayerMoveTest::RunTest(const FString& Parameters)
{
	UMaterialFunctionMaterialLayer* Bottom =
		NewObject<UMaterialFunctionMaterialLayer>(GetTransientPackage());
	UMaterialFunctionMaterialLayer* Middle =
		NewObject<UMaterialFunctionMaterialLayer>(GetTransientPackage());
	UMaterialFunctionMaterialLayer* Top =
		NewObject<UMaterialFunctionMaterialLayer>(GetTransientPackage());

	FMaterialLayersFunctions Layers;
	Layers.AddDefaultBackgroundLayer();
	Layers.Layers[0] = Bottom;
	Layers.Layers[Layers.AppendBlendedLayer()] = Middle;
	Layers.Layers[Layers.AppendBlendedLayer()] = Top;
	Layers.EditorOnly.LayerNames[1] = FText::FromString(TEXT("Middle"));
	Layers.EditorOnly.LayerNames[2] = FText::FromString(TEXT("Top"));

	// Moving carries the layer's name with it, which is what makes the stack
	// legible after a reorder.
	Layers.MoveBlendedLayer(2, 1);
	TestTrue(TEXT("the top layer moved down"), Layers.Layers[1] == Top);
	TestTrue(TEXT("the middle layer moved up"), Layers.Layers[2] == Middle);
	TestEqual(TEXT("names moved with them"),
		Layers.EditorOnly.LayerNames[1].ToString(), FString(TEXT("Top")));
	TestTrue(TEXT("the background layer stayed put"), Layers.Layers[0] == Bottom);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
