// Material graph editing: expressions (nodes), links between them, links into
// the material's own inputs (BaseColor, Roughness, ...), recompile, layout.
//
// Expression options (constants, parameter names, texture references) are
// plain UPROPERTYs, so they are edited with set_property on the reported
// expression path — the graph route only deals with structure.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "MaterialExpressionIO.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "SceneTypes.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace
	{
		UMaterial* MaterialOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("material"), Path, Responder,
				TEXT("a Material asset path, e.g. /Game/Materials/M_Thing (instances have no graph)")))
			{
				return nullptr;
			}
			UObject* Object = ResolveObject(Path);
			if (Object == nullptr && !Path.Contains(TEXT(".")))
			{
				Object = ResolveObject(FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path)));
			}
			UMaterial* Material = Cast<UMaterial>(Object);
			if (Material == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("material_not_found"),
					FString::Printf(TEXT("no Material at '%s' — material_ops create makes one"), *Path));
			}
			return Material;
		}

		/// "Constant3Vector", "MaterialExpressionConstant3Vector", or a full class path.
		UClass* ExpressionClassOrError(const FString& Spec, const TSharedRef<FMcpResponder>& Responder)
		{
			UClass* Class = Spec.IsEmpty() ? nullptr : ResolveClass(Spec);
			if (Class == nullptr && !Spec.IsEmpty() && !Spec.StartsWith(TEXT("MaterialExpression")))
			{
				Class = ResolveClass(TEXT("MaterialExpression") + Spec);
			}
			if (Class == nullptr || !Class->IsChildOf(UMaterialExpression::StaticClass())
				|| Class->HasAnyClassFlags(CLASS_Abstract))
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("expression_class_not_found"),
					FString::Printf(
						TEXT("'%s' is not a concrete MaterialExpression class — see list_expression_classes"), *Spec));
				return nullptr;
			}
			return Class;
		}

		/// Expressions are addressed by object name ("MaterialExpressionConstant3Vector_0")
		/// or full object path.
		UMaterialExpression* FindExpression(UMaterial* Material, const FString& Spec)
		{
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (Expression != nullptr && (Expression->GetName() == Spec || Expression->GetPathName() == Spec))
				{
					return Expression;
				}
			}
			return nullptr;
		}

		UMaterialExpression* ExpressionOrError(
			UMaterial* Material, const TSharedRef<FJsonObject>& Body, const TCHAR* Field,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			Body->TryGetStringField(Field, Spec);
			if (Spec.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					FString::Printf(TEXT("'%s' (expression name from get_graph) is required"), Field));
				return nullptr;
			}
			UMaterialExpression* Expression = FindExpression(Material, Spec);
			if (Expression == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("expression_not_found"),
					FString::Printf(TEXT("no expression '%s' in %s — see get_graph"), *Spec, *Material->GetName()));
			}
			return Expression;
		}

		bool PropertyOrError(const FString& Spec, EMaterialProperty& Out, const TSharedRef<FMcpResponder>& Responder)
		{
			const UEnum* Enum = StaticEnum<EMaterialProperty>();
			const FString Name = Spec.StartsWith(TEXT("MP_")) ? Spec : TEXT("MP_") + Spec;
			const int64 Value = Enum->GetValueByNameString(Name);
			if (Value == INDEX_NONE || Value == MP_MAX)
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_material_property"),
					FString::Printf(
						TEXT("'%s' is not a material input — use BaseColor, Metallic, Specular, Roughness, Anisotropy, ")
						TEXT("EmissiveColor, Opacity, OpacityMask, Normal, Tangent, WorldPositionOffset, SubsurfaceColor, ")
						TEXT("AmbientOcclusion, Refraction, PixelDepthOffset, ShadingModel, or CustomData0/1"),
						*Spec));
				return false;
			}
			Out = static_cast<EMaterialProperty>(Value);
			return true;
		}

		TSharedRef<FJsonObject> ExpressionToJson(UMaterialExpression* Expression)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Expression->GetName());
			Object->SetStringField(TEXT("path"), Expression->GetPathName());
			Object->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
			TArray<FString> Captions;
			Expression->GetCaption(Captions);
			Object->SetStringField(TEXT("caption"), FString::Join(Captions, TEXT(" ")));
			Object->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
			Object->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);

			TArray<TSharedPtr<FJsonValue>> Inputs;
			int32 Index = 0;
			for (FExpressionInput* Input : Expression->GetInputsView())
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Expression->GetInputName(Index).ToString());
				if (Input != nullptr && Input->Expression != nullptr)
				{
					Item->SetStringField(TEXT("from"), Input->Expression->GetName());
					Item->SetNumberField(TEXT("from_output"), Input->OutputIndex);
				}
				Inputs.Add(MakeShared<FJsonValueObject>(Item));
				++Index;
			}
			Object->SetArrayField(TEXT("inputs"), Inputs);

			// Most outputs are unnamed (the editor shows a colour dot), so they
			// are addressed by index; the mask says which channels they carry.
			TArray<TSharedPtr<FJsonValue>> Outputs;
			int32 OutputIndex = 0;
			for (const FExpressionOutput& Output : Expression->GetOutputs())
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), OutputIndex++);
				if (!Output.OutputName.IsNone())
				{
					Item->SetStringField(TEXT("name"), Output.OutputName.ToString());
				}
				if (Output.Mask)
				{
					FString Mask;
					if (Output.MaskR) { Mask += TEXT("R"); }
					if (Output.MaskG) { Mask += TEXT("G"); }
					if (Output.MaskB) { Mask += TEXT("B"); }
					if (Output.MaskA) { Mask += TEXT("A"); }
					Item->SetStringField(TEXT("mask"), Mask);
				}
				Outputs.Add(MakeShared<FJsonValueObject>(Item));
			}
			Object->SetArrayField(TEXT("outputs"), Outputs);
			return Object;
		}

		/// Optional numeric output selector ("from_output_index"); INDEX_NONE when absent.
		int32 OutputIndexOf(const TSharedRef<FJsonObject>& Body)
		{
			double Index = 0.0;
			return Body->TryGetNumberField(TEXT("from_output_index"), Index) ? static_cast<int32>(Index) : INDEX_NONE;
		}

		bool ValidOutputIndex(const UMaterialExpression* Expression, int32 Index)
		{
			return Index >= 0 && Index < const_cast<UMaterialExpression*>(Expression)->GetOutputs().Num();
		}

		TSharedRef<FJsonObject> GraphToJson(UMaterial* Material)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("material"), Material->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Expressions;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (Expression != nullptr)
				{
					Expressions.Add(MakeShared<FJsonValueObject>(ExpressionToJson(Expression)));
				}
			}
			Data->SetArrayField(TEXT("expressions"), Expressions);

			// Which expression feeds each material input.
			static const EMaterialProperty Properties[] = {
				MP_BaseColor, MP_Metallic, MP_Specular, MP_Roughness, MP_Anisotropy, MP_EmissiveColor,
				MP_Opacity, MP_OpacityMask, MP_Normal, MP_Tangent, MP_WorldPositionOffset,
				MP_SubsurfaceColor, MP_AmbientOcclusion, MP_Refraction, MP_PixelDepthOffset,
				MP_ShadingModel, MP_CustomData0, MP_CustomData1};
			const UEnum* Enum = StaticEnum<EMaterialProperty>();
			const TSharedRef<FJsonObject> Connections = MakeShared<FJsonObject>();
			for (EMaterialProperty Property : Properties)
			{
				if (UMaterialExpression* Node = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, Property))
				{
					Connections->SetStringField(
						Enum->GetNameStringByValue(Property).RightChop(3), Node->GetName());
				}
			}
			Data->SetObjectField(TEXT("material_inputs"), Connections);
			Data->SetStringField(TEXT("shading_model"),
				StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(Material->GetShadingModels().GetFirstShadingModel()));
			Data->SetStringField(TEXT("blend_mode"),
				StaticEnum<EBlendMode>()->GetNameStringByValue(Material->GetBlendMode()));
			return Data;
		}

		void MarkChanged(UMaterial* Material)
		{
			Material->MarkPackageDirty();
		}
	}

	void RegisterMaterialGraphRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/materials/graph"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_expression_classes"))
				{
					FString Filter;
					Body->TryGetStringField(TEXT("filter"), Filter);
					TArray<TSharedPtr<FJsonValue>> Classes;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!Class->IsChildOf(UMaterialExpression::StaticClass())
							|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
						{
							continue;
						}
						const FString Short = Class->GetName().Replace(TEXT("MaterialExpression"), TEXT(""));
						if (!Filter.IsEmpty() && !Short.Contains(Filter))
						{
							continue;
						}
						Classes.Add(MakeShared<FJsonValueString>(Short));
						if (Classes.Num() >= 500) { break; }
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), Classes.Num());
					Data->SetArrayField(TEXT("expression_classes"), Classes);
					Responder->Ok(Data);
					return;
				}

				UMaterial* Material = MaterialOrError(Body, Responder);
				if (!Material) { return; }

				if (Operation == TEXT("get_graph"))
				{
					Responder->Ok(GraphToJson(Material));
					return;
				}

				if (Operation == TEXT("recompile"))
				{
					const TArray<FString> Errors = UMaterialEditingLibrary::RecompileMaterial(Material);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("compiled"), Errors.IsEmpty());
					TArray<TSharedPtr<FJsonValue>> ErrorValues;
					for (const FString& Error : Errors)
					{
						ErrorValues.Add(MakeShared<FJsonValueString>(Error));
					}
					Data->SetArrayField(TEXT("errors"), ErrorValues);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "EditMaterialGraph", "McpLink Edit Material Graph"));
				Material->Modify();

				if (Operation == TEXT("add_expression"))
				{
					FString ClassSpec;
					Body->TryGetStringField(TEXT("class"), ClassSpec);
					UClass* Class = ExpressionClassOrError(ClassSpec, Responder);
					if (!Class) { return; }
					double X = 0, Y = 0;
					Body->TryGetNumberField(TEXT("x"), X);
					Body->TryGetNumberField(TEXT("y"), Y);
					UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpression(
						Material, Class, static_cast<int32>(X), static_cast<int32>(Y));
					if (Expression == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("expression_not_created"),
							TEXT("CreateMaterialExpression returned null"));
						return;
					}
					MarkChanged(Material);
					const TSharedRef<FJsonObject> Data = ExpressionToJson(Expression);
					Data->SetStringField(TEXT("message"),
						TEXT("set its options with set_property on 'path' (e.g. Constant, DefaultValue, ParameterName, Texture)"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("delete_expression"))
				{
					UMaterialExpression* Expression = ExpressionOrError(Material, Body, TEXT("expression"), Responder);
					if (!Expression) { return; }
					const FString Name = Expression->GetName();
					UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
					MarkChanged(Material);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Name);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("connect"))
				{
					UMaterialExpression* From = ExpressionOrError(Material, Body, TEXT("from"), Responder);
					if (!From) { return; }
					UMaterialExpression* To = ExpressionOrError(Material, Body, TEXT("to"), Responder);
					if (!To) { return; }
					FString FromOutput, ToInput;
					Body->TryGetStringField(TEXT("from_output"), FromOutput);
					Body->TryGetStringField(TEXT("to_input"), ToInput);
					if (ToInput.IsEmpty() && To->CountInputs() > 0)
					{
						ToInput = To->GetInputName(0).ToString();
					}
					const int32 FromIndex = OutputIndexOf(Body);
					bool bConnected = false;
					if (FromIndex != INDEX_NONE)
					{
						// Unnamed outputs (channel masks) can only be picked by index.
						if (!ValidOutputIndex(From, FromIndex))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_output_index"),
								FString::Printf(TEXT("%s has %d outputs"), *From->GetName(), From->GetOutputs().Num()));
							return;
						}
						for (int32 Index = 0; Index < To->CountInputs(); ++Index)
						{
							if (To->GetInputName(Index).ToString().Equals(ToInput, ESearchCase::IgnoreCase))
							{
								if (FExpressionInput* Input = To->GetInput(Index))
								{
									To->Modify();
									Input->Connect(FromIndex, From);
									bConnected = true;
								}
							}
						}
					}
					else
					{
						bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput);
					}
					if (!bConnected)
					{
						TArray<FString> InputNames;
						for (int32 Index = 0; Index < To->CountInputs(); ++Index)
						{
							InputNames.Add(To->GetInputName(Index).ToString());
						}
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("connect_failed"),
							FString::Printf(TEXT("could not connect %s.%s -> %s.%s — inputs on %s: %s"),
								*From->GetName(), FromOutput.IsEmpty() ? TEXT("(default)") : *FromOutput,
								*To->GetName(), *ToInput, *To->GetName(), *FString::Join(InputNames, TEXT(", "))));
						return;
					}
					MarkChanged(Material);
					Responder->Ok(ExpressionToJson(To));
					return;
				}

				if (Operation == TEXT("connect_property") || Operation == TEXT("disconnect_property"))
				{
					FString PropertySpec;
					Body->TryGetStringField(TEXT("property"), PropertySpec);
					EMaterialProperty Property;
					if (!PropertyOrError(PropertySpec, Property, Responder)) { return; }
					if (Operation == TEXT("connect_property"))
					{
						UMaterialExpression* From = ExpressionOrError(Material, Body, TEXT("from"), Responder);
						if (!From) { return; }
						FString FromOutput;
						Body->TryGetStringField(TEXT("from_output"), FromOutput);
						const int32 FromIndex = OutputIndexOf(Body);
						bool bConnected = false;
						if (FromIndex != INDEX_NONE)
						{
							FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
							if (Input != nullptr && ValidOutputIndex(From, FromIndex))
							{
								Input->Connect(FromIndex, From);
								bConnected = true;
							}
						}
						else
						{
							bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOutput, Property);
						}
						if (!bConnected)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("connect_failed"),
								FString::Printf(TEXT("could not connect %s to %s"), *From->GetName(), *PropertySpec));
							return;
						}
					}
					else if (FExpressionInput* Input = Material->GetExpressionInputForProperty(Property))
					{
						Input->Expression = nullptr;
						Input->OutputIndex = 0;
					}
					MarkChanged(Material);
					Responder->Ok(GraphToJson(Material));
					return;
				}

				if (Operation == TEXT("layout"))
				{
					UMaterialEditingLibrary::LayoutMaterialExpressions(Material);
					MarkChanged(Material);
					Responder->Ok(GraphToJson(Material));
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_expression_classes, get_graph, add_expression, ")
						TEXT("delete_expression, connect, connect_property, disconnect_property, layout, or recompile"),
						*Operation));
			});
	}
}
