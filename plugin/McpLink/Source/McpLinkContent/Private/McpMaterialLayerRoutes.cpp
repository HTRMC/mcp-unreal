// Material Layers: the layer/blend stack, and the two function assets it is
// built out of.
//
// A layer stack is a FMaterialLayersFunctions — parallel arrays of layer
// functions and the blend functions between them, plus editor-only names and
// visibility. It lives in two places, and this route edits both:
//
//   - on a UMaterial, as the DefaultLayers of a MaterialAttributeLayers
//     expression in its graph (material_graph add_expression places one);
//   - on a UMaterialInstanceConstant, as an override of its parent's stack,
//     through Get/SetMaterialLayers.
//
// Layer 0 is the background layer and has no blend under it, so Blends[i] is
// what puts Layers[i + 1] over everything below it and there is always one
// fewer blend than layer. AppendBlendedLayer and RemoveBlendedLayerAt keep the
// two arrays and the editor-only arrays in step, so the stack is only ever
// resized through them.
//
// The layer and blend assets themselves are ordinary Material Functions with a
// usage flag, so material_function's add_expression / connect / update / save
// author their bodies. What the Material Editor adds when you *open* a new one
// — the MaterialAttributes inputs and the layer output — is created here
// instead, since a headless pipeline never opens the asset.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialExpressionMaterialLayerOutput.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialLayersFunctions.h"
#include "StaticParameterSet.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace MaterialLayers
	{
		/// The stack an operation edits, and how to write it back. A material
		/// owns its stack outright; an instance's is an override that only
		/// exists once SetMaterialLayers has taken it.
		struct FStackTarget
		{
			UMaterialExpressionMaterialAttributeLayers* Expression = nullptr;
			UMaterialInstanceConstant* Instance = nullptr;
			UMaterial* Material = nullptr;
			FMaterialLayersFunctions Layers;

			bool IsValid() const { return Expression != nullptr || Instance != nullptr; }
		};

		UMaterialFunctionInterface* LayerFunctionOrError(const TSharedRef<FJsonObject>& Body,
			const TCHAR* Field, EMaterialFunctionUsage Usage, bool bRequired,
			const TSharedRef<FMcpResponder>& Responder, bool& bOutFailed)
		{
			bOutFailed = false;
			FString Path;
			if (!Body->TryGetStringField(Field, Path) || Path.IsEmpty())
			{
				if (bRequired)
				{
					bOutFailed = true;
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
						FString::Printf(TEXT("'%s' is required — a Material Layer%s asset path"), Field,
							Usage == EMaterialFunctionUsage::MaterialLayerBlend ? TEXT(" Blend") : TEXT("")));
				}
				return nullptr;
			}
			UMaterialFunctionInterface* Function =
				Cast<UMaterialFunctionInterface>(ResolveAsset(Path));
			if (Function == nullptr)
			{
				bOutFailed = true;
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("function_not_found"),
					FString::Printf(TEXT("no Material Function at '%s'"), *Path));
				return nullptr;
			}
			if (Function->GetMaterialFunctionUsage() != Usage)
			{
				bOutFailed = true;
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrong_usage"),
					FString::Printf(
						TEXT("'%s' is a %s function, but a layer stack needs a %s there — ")
						TEXT("material_layers create_layer and create_blend make the right kind"),
						*Path,
						Function->GetMaterialFunctionUsage() == EMaterialFunctionUsage::MaterialLayer
							? TEXT("Material Layer")
							: Function->GetMaterialFunctionUsage()
									== EMaterialFunctionUsage::MaterialLayerBlend
								? TEXT("Material Layer Blend")
								: TEXT("plain Material"),
						Usage == EMaterialFunctionUsage::MaterialLayer ? TEXT("Material Layer")
																	   : TEXT("Material Layer Blend")));
				return nullptr;
			}
			return Function;
		}

		/// Resolve "asset" to whichever of the two stack holders it is, and read
		/// the current stack out of it.
		bool ResolveStack(const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder, FStackTarget& Out)
		{
			FString Path;
			if (!RequireString(Body, TEXT("asset"), Path, Responder,
					TEXT("a Material or Material Instance path, e.g. /Game/Materials/M_Terrain")))
			{
				return false;
			}
			UObject* Asset = ResolveAsset(Path);
			if (Asset == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
					FString::Printf(TEXT("no asset at '%s'"), *Path));
				return false;
			}

			if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Asset))
			{
				Out.Instance = Instance;
				// False just means the instance has no override yet; the parent's
				// stack is the honest starting point for editing one in.
				if (!Instance->GetMaterialLayers(Out.Layers))
				{
					Out.Layers.Empty();
				}
				return true;
			}

			UMaterial* Material = Cast<UMaterial>(Asset);
			if (Material == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrong_asset_type"),
					FString::Printf(
						TEXT("'%s' is a %s — a layer stack lives on a Material or a Material Instance"),
						*Path, *Asset->GetClass()->GetName()));
				return false;
			}
			Out.Material = Material;

			// A material normally holds exactly one layers node — it has no
			// parameter name of its own in 5.8 — so "expression" only matters
			// for the unusual material that has several.
			FString Wanted;
			Body->TryGetStringField(TEXT("expression"), Wanted);
			TArray<UMaterialExpressionMaterialAttributeLayers*> Found;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (UMaterialExpressionMaterialAttributeLayers* Layers =
						Cast<UMaterialExpressionMaterialAttributeLayers>(Expression))
				{
					Found.Add(Layers);
				}
			}
			if (Found.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("no_layers_expression"),
					FString::Printf(
						TEXT("'%s' has no Material Attribute Layers node — add one with material_graph ")
						TEXT("add_expression {\"class\": \"MaterialAttributeLayers\"} and connect its ")
						TEXT("output to the material's Material Attributes"),
						*Path));
				return false;
			}
			for (UMaterialExpressionMaterialAttributeLayers* Candidate : Found)
			{
				if (Wanted.IsEmpty() || Candidate->GetName() == Wanted
					|| Candidate->GetPathName() == Wanted)
				{
					Out.Expression = Candidate;
					break;
				}
			}
			if (Out.Expression == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("expression_not_found"),
					FString::Printf(
						TEXT("'%s' has no layers node '%s' — info lists the ones it has"),
						*Path, *Wanted));
				return false;
			}
			Out.Layers = Out.Expression->DefaultLayers;
			return true;
		}

		/// Write an edited stack back where it came from. A material's node is a
		/// plain property assignment plus a recompile; an instance's goes through
		/// SetMaterialLayers, which is what rebuilds its static permutation.
		bool CommitStack(FStackTarget& Target, FString& OutError)
		{
			if (Target.Instance != nullptr)
			{
				if (!Target.Instance->SetMaterialLayers(Target.Layers))
				{
					OutError = TEXT("SetMaterialLayers rejected the stack — it is unchanged");
					return false;
				}
				Target.Instance->PostEditChange();
				Target.Instance->MarkPackageDirty();
				return true;
			}
			Target.Expression->DefaultLayers = Target.Layers;
			Target.Expression->Modify();
			UMaterialEditingLibrary::RecompileMaterial(Target.Material);
			Target.Material->MarkPackageDirty();
			return true;
		}

		FString FunctionPath(const UMaterialFunctionInterface* Function)
		{
			return Function != nullptr ? Function->GetPathName() : FString();
		}

		TSharedRef<FJsonObject> StackToJson(const FMaterialLayersFunctions& Layers)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Items;
			for (int32 Index = 0; Index < Layers.Layers.Num(); ++Index)
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Index);
				Item->SetStringField(TEXT("layer"), FunctionPath(Layers.Layers[Index]));
				// Layer 0 sits at the bottom, so nothing blends it.
				if (Layers.Blends.IsValidIndex(Index - 1))
				{
					Item->SetStringField(TEXT("blend"), FunctionPath(Layers.Blends[Index - 1]));
				}
				if (Layers.EditorOnly.LayerNames.IsValidIndex(Index))
				{
					Item->SetStringField(TEXT("name"),
						Layers.EditorOnly.LayerNames[Index].ToString());
				}
				if (Layers.EditorOnly.LayerStates.IsValidIndex(Index))
				{
					Item->SetBoolField(TEXT("visible"), Layers.EditorOnly.LayerStates[Index]);
				}
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}
			Object->SetNumberField(TEXT("layer_count"), Layers.Layers.Num());
			Object->SetArrayField(TEXT("layers"), Items);
			return Object;
		}

		/// The nodes the Material Editor puts in a freshly created layer or blend
		/// the first time it is opened. Created here so a stack built headlessly
		/// has something to compile.
		void SeedLayerFunction(UMaterialFunction* Function, bool bBlend)
		{
			UMaterialExpression* Output = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
				Function, UMaterialExpressionMaterialLayerOutput::StaticClass(), 300, 300);
			if (Output != nullptr)
			{
				Output->bCollapsed = true;
			}

			auto AddInput = [Function](const TCHAR* Name, int32 Y, EBlendInputRelevance Relevance)
			{
				UMaterialExpressionFunctionInput* Input =
					Cast<UMaterialExpressionFunctionInput>(
						UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
							Function, UMaterialExpressionFunctionInput::StaticClass(), -300, Y));
				if (Input != nullptr)
				{
					Input->InputType = FunctionInput_MaterialAttributes;
					Input->InputName = FName(Name);
					Input->bUsePreviewValueAsDefault = true;
					Input->BlendInputRelevance = Relevance;
				}
			};

			if (bBlend)
			{
				// Top sits below Bottom on the graph, matching B on a blend node.
				AddInput(BottomMaterialBlendInputName, 200, EBlendInputRelevance::Bottom);
				AddInput(TopMaterialBlendInputName, 400, EBlendInputRelevance::Top);
			}
			else
			{
				AddInput(TEXT("Material Attributes"), 300, EBlendInputRelevance::General);
			}
			UMaterialEditingLibrary::UpdateMaterialFunction(Function);
		}

		/// Layer index bounds, phrased the way the caller needs to fix them.
		bool RequireLayerIndex(const TSharedRef<FJsonObject>& Body, const FStackTarget& Target,
			const TSharedRef<FMcpResponder>& Responder, int32& OutIndex)
		{
			OutIndex = IntOr(Body, TEXT("index"), -1);
			if (!Target.Layers.Layers.IsValidIndex(OutIndex))
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_index"),
					FString::Printf(TEXT("'index' must be 0..%d — this stack has %d layer(s)"),
						Target.Layers.Layers.Num() - 1, Target.Layers.Layers.Num()));
				return false;
			}
			return true;
		}
	}

	using namespace MaterialLayers;

	void RegisterMaterialLayerRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/materials/layers"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create_layer") || Operation == TEXT("create_blend"))
				{
					const bool bBlend = Operation == TEXT("create_blend");
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							bBlend ? TEXT("e.g. /Game/Materials/MLB_HeightBlend")
								   : TEXT("e.g. /Game/Materials/ML_Rock")))
					{
						return;
					}
					if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMaterialLayer", "McpLink Create Material Layer"));
					UPackage* Package = CreatePackage(*Path);
					const FName Name(*FPackageName::GetShortName(Path));
					UMaterialFunction* Function = bBlend
						? static_cast<UMaterialFunction*>(NewObject<UMaterialFunctionMaterialLayerBlend>(
							  Package, Name, RF_Public | RF_Standalone | RF_Transactional))
						: static_cast<UMaterialFunction*>(NewObject<UMaterialFunctionMaterialLayer>(
							  Package, Name, RF_Public | RF_Standalone | RF_Transactional));
					Function->SetMaterialFunctionUsage(bBlend
							? EMaterialFunctionUsage::MaterialLayerBlend
							: EMaterialFunctionUsage::MaterialLayer);
					FString Description;
					if (Body->TryGetStringField(TEXT("description"), Description))
					{
						Function->Description = Description;
					}
					Function->bExposeToLibrary = BoolOr(Body, TEXT("expose_to_library"), true);
					if (BoolOr(Body, TEXT("seed_nodes"), true))
					{
						SeedLayerFunction(Function, bBlend);
					}
					FAssetRegistryModule::AssetCreated(Function);
					Package->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("function"), Function->GetPathName());
					Data->SetStringField(TEXT("usage"),
						bBlend ? TEXT("MaterialLayerBlend") : TEXT("MaterialLayer"));
					Data->SetStringField(TEXT("message"), bBlend
							? TEXT("created with 'Top Layer' and 'Bottom Layer' MaterialAttributes ")
							  TEXT("inputs and a layer output — author the body with material_function ")
							  TEXT("add_expression / connect, then update and save")
							: TEXT("created with a 'Material Attributes' input and a layer output — ")
							  TEXT("author the body with material_function add_expression / connect, ")
							  TEXT("then update and save"));
					Responder->Ok(Data);
					return;
				}

				FStackTarget Target;
				if (!ResolveStack(Body, Responder, Target))
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = StackToJson(Target.Layers);
					if (Target.Instance != nullptr)
					{
						Data->SetStringField(TEXT("asset"), Target.Instance->GetPathName());
						Data->SetStringField(TEXT("kind"), TEXT("material_instance"));
						Data->SetStringField(TEXT("parent"),
							Target.Instance->Parent != nullptr
								? Target.Instance->Parent->GetPathName()
								: FString());
						// GetMaterialLayers walks up to the parent, so the stack
						// above may be inherited; only the instance's own static
						// parameters say whether it overrides one.
						Data->SetBoolField(TEXT("overrides_parent"),
							Target.Instance->GetStaticParameters().bHasMaterialLayers != 0);
					}
					else
					{
						Data->SetStringField(TEXT("asset"), Target.Material->GetPathName());
						Data->SetStringField(TEXT("kind"), TEXT("material"));
						Data->SetStringField(TEXT("expression"), Target.Expression->GetPathName());
						TArray<TSharedPtr<FJsonValue>> Nodes;
						for (UMaterialExpression* Expression : Target.Material->GetExpressions())
						{
							if (const UMaterialExpressionMaterialAttributeLayers* Node =
									Cast<UMaterialExpressionMaterialAttributeLayers>(Expression))
							{
								Nodes.Add(MakeShared<FJsonValueString>(Node->GetName()));
							}
						}
						Data->SetArrayField(TEXT("layers_nodes"), Nodes);
					}
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditMaterialLayers", "McpLink Edit Material Layers"));
				if (Target.Instance != nullptr)
				{
					Target.Instance->Modify();
				}
				else
				{
					Target.Material->Modify();
				}

				bool bFailed = false;
				if (Operation == TEXT("add_layer"))
				{
					UMaterialFunctionInterface* Layer = LayerFunctionOrError(Body, TEXT("layer"),
						EMaterialFunctionUsage::MaterialLayer, false, Responder, bFailed);
					if (bFailed) { return; }
					UMaterialFunctionInterface* Blend = LayerFunctionOrError(Body, TEXT("blend"),
						EMaterialFunctionUsage::MaterialLayerBlend, false, Responder, bFailed);
					if (bFailed) { return; }

					// A stack that has never been touched has no background layer
					// to blend against, so the first add_layer supplies one.
					if (Target.Layers.Layers.IsEmpty())
					{
						Target.Layers.AddDefaultBackgroundLayer();
						Target.Layers.Layers[0] = Layer;
					}
					else
					{
						const int32 Index = Target.Layers.AppendBlendedLayer();
						Target.Layers.Layers[Index] = Layer;
						Target.Layers.Blends[Index - 1] = Blend;
					}
					FString Name;
					if (Body->TryGetStringField(TEXT("name"), Name)
						&& Target.Layers.EditorOnly.LayerNames.IsValidIndex(
							Target.Layers.Layers.Num() - 1))
					{
						Target.Layers.EditorOnly.LayerNames[Target.Layers.Layers.Num() - 1] =
							FText::FromString(Name);
					}
				}
				else if (Operation == TEXT("set_layer") || Operation == TEXT("set_blend")
					|| Operation == TEXT("set_layer_name")
					|| Operation == TEXT("set_layer_visibility")
					|| Operation == TEXT("remove_layer"))
				{
					int32 Index = 0;
					if (!RequireLayerIndex(Body, Target, Responder, Index))
					{
						return;
					}
					if (Operation == TEXT("set_layer"))
					{
						UMaterialFunctionInterface* Layer = LayerFunctionOrError(Body, TEXT("layer"),
							EMaterialFunctionUsage::MaterialLayer, true, Responder, bFailed);
						if (bFailed) { return; }
						Target.Layers.Layers[Index] = Layer;
					}
					else if (Operation == TEXT("set_blend"))
					{
						if (Index == 0)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_blend"),
								TEXT("layer 0 is the background layer and has nothing under it to ")
								TEXT("blend with — set_blend applies to layer 1 and above"));
							return;
						}
						UMaterialFunctionInterface* Blend = LayerFunctionOrError(Body, TEXT("blend"),
							EMaterialFunctionUsage::MaterialLayerBlend, true, Responder, bFailed);
						if (bFailed) { return; }
						Target.Layers.Blends[Index - 1] = Blend;
					}
					else if (Operation == TEXT("set_layer_name"))
					{
						FString Name;
						if (!RequireString(Body, TEXT("name"), Name, Responder,
								TEXT("the label the layer shows in the stack")))
						{
							return;
						}
						if (Target.Layers.EditorOnly.LayerNames.IsValidIndex(Index))
						{
							Target.Layers.EditorOnly.LayerNames[Index] = FText::FromString(Name);
						}
					}
					else if (Operation == TEXT("set_layer_visibility"))
					{
						if (Index == 0)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("always_visible"),
								TEXT("the background layer cannot be hidden — hide the layers above it"));
							return;
						}
						Target.Layers.SetBlendedLayerVisibility(
							Index, BoolOr(Body, TEXT("visible"), true));
					}
					else
					{
						if (Index == 0)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("cannot_remove"),
								TEXT("layer 0 is the background layer and is what the stack blends ")
								TEXT("onto — replace it with set_layer instead of removing it"));
							return;
						}
						Target.Layers.RemoveBlendedLayerAt(Index);
					}
				}
				else if (Operation == TEXT("move_layer"))
				{
					const int32 From = IntOr(Body, TEXT("from_index"), -1);
					const int32 To = IntOr(Body, TEXT("to_index"), -1);
					if (!Target.Layers.Layers.IsValidIndex(From)
						|| !Target.Layers.Layers.IsValidIndex(To))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_index"),
							FString::Printf(
								TEXT("'from_index' and 'to_index' must be 0..%d"),
								Target.Layers.Layers.Num() - 1));
						return;
					}
					if (From == 0 || To == 0)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("cannot_move"),
							TEXT("layer 0 is the background layer and stays at the bottom"));
						return;
					}
					Target.Layers.MoveBlendedLayer(From, To);
				}
				else
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
						FString::Printf(
							TEXT("unknown operation '%s' — use create_layer, create_blend, info, ")
							TEXT("add_layer, set_layer, set_blend, remove_layer, move_layer, ")
							TEXT("set_layer_name or set_layer_visibility"),
							*Operation));
					return;
				}

				FString Error;
				if (!CommitStack(Target, Error))
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("commit_failed"), Error);
					return;
				}
				const TSharedRef<FJsonObject> Data = StackToJson(Target.Layers);
				Data->SetStringField(TEXT("asset"), Target.Instance != nullptr
						? Target.Instance->GetPathName()
						: Target.Material->GetPathName());
				Data->SetStringField(TEXT("message"),
					TEXT("stack updated — save the asset to keep it"));
				Responder->Ok(Data);
			});
	}
}
