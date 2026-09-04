// Material Functions: the reusable sub-graphs a material calls into.
//
// material_graph edits a UMaterial's own graph; a function is the same kind of
// expression graph in its own asset, with FunctionInput and FunctionOutput
// expressions standing in for its parameters and results. UMaterialEditingLibrary
// has a parallel set of calls for them, which is what this route uses.
//
// Expression options — constants, parameter names, input names and sort
// priorities — are plain UPROPERTYs on the reported expression path, so
// set_property tunes them, exactly as for a material graph.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialFunction.h"
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
	namespace MaterialFunctions
	{
		UMaterialFunction* FunctionOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("function"), Path, Responder,
					TEXT("a Material Function asset path, e.g. /Game/Materials/MF_Blend")))
			{
				return nullptr;
			}
			UMaterialFunction* Function = Cast<UMaterialFunction>(ResolveAsset(Path));
			if (Function == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("function_not_found"),
					FString::Printf(
						TEXT("no Material Function at '%s' — material_function create makes one"), *Path));
			}
			return Function;
		}

		/// "Add", "MaterialExpressionAdd" or a full class path.
		UClass* FunctionExpressionClass(const FString& Spec)
		{
			UClass* Class = Spec.IsEmpty() ? nullptr : ResolveClass(Spec);
			if (Class == nullptr && !Spec.IsEmpty() && !Spec.StartsWith(TEXT("MaterialExpression")))
			{
				Class = ResolveClass(TEXT("MaterialExpression") + Spec);
			}
			if (Class == nullptr || !Class->IsChildOf(UMaterialExpression::StaticClass())
				|| Class->HasAnyClassFlags(CLASS_Abstract))
			{
				return nullptr;
			}
			return Class;
		}

		UMaterialExpression* FindExpression(UMaterialFunction* Function, const FString& Spec)
		{
			for (UMaterialExpression* Expression :
				UMaterialEditingLibrary::GetMaterialFunctionExpressions(Function))
			{
				if (Expression != nullptr
					&& (Expression->GetName() == Spec || Expression->GetPathName() == Spec))
				{
					return Expression;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> FunctionExpressionToJson(UMaterialExpression* Expression)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Expression->GetName());
			Object->SetStringField(TEXT("class"),
				Expression->GetClass()->GetName().Replace(TEXT("MaterialExpression"), TEXT("")));
			// Expression options are properties on this path.
			Object->SetStringField(TEXT("path"), Expression->GetPathName());
			Object->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
			Object->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
			if (const UMaterialExpressionFunctionInput* Input =
					Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				Object->SetStringField(TEXT("input_name"), Input->InputName.ToString());
				Object->SetNumberField(TEXT("sort_priority"), Input->SortPriority);
			}
			if (const UMaterialExpressionFunctionOutput* Output =
					Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				Object->SetStringField(TEXT("output_name"), Output->OutputName.ToString());
				Object->SetNumberField(TEXT("sort_priority"), Output->SortPriority);
			}
			return Object;
		}
	}

	using namespace MaterialFunctions;

	void RegisterMaterialFunctionRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/materials/function"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Materials/MF_Blend")))
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
						NSLOCTEXT("McpLink", "CreateMaterialFunction", "McpLink Create Material Function"));
					UPackage* Package = CreatePackage(*Path);
					UMaterialFunction* Function = NewObject<UMaterialFunction>(Package,
						FName(*FPackageName::GetShortName(Path)),
						RF_Public | RF_Standalone | RF_Transactional);
					FString Description;
					if (Body->TryGetStringField(TEXT("description"), Description))
					{
						Function->Description = Description;
					}
					// Off by default a function never appears in the palette,
					// which is almost never what someone authoring one wants.
					Function->SetMaterialFunctionUsage(EMaterialFunctionUsage::Default);
					Function->bExposeToLibrary = BoolOr(Body, TEXT("expose_to_library"), true);
					FAssetRegistryModule::AssetCreated(Function);
					Package->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("function"), Function->GetPathName());
					Data->SetStringField(TEXT("message"),
						TEXT("created empty — add FunctionInput and FunctionOutput expressions, wire the ")
						TEXT("body between them, then update and save"));
					Responder->Ok(Data);
					return;
				}

				UMaterialFunction* Function = FunctionOrError(Body, Responder);
				if (Function == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("function"), Function->GetPathName());
					Data->SetStringField(TEXT("description"), Function->Description);
					Data->SetBoolField(TEXT("exposed_to_library"), Function->bExposeToLibrary);
					TArray<TSharedPtr<FJsonValue>> Expressions;
					for (UMaterialExpression* Expression :
						UMaterialEditingLibrary::GetMaterialFunctionExpressions(Function))
					{
						if (Expression != nullptr)
						{
							Expressions.Add(
								MakeShared<FJsonValueObject>(FunctionExpressionToJson(Expression)));
						}
					}
					Data->SetNumberField(TEXT("expression_count"), Expressions.Num());
					Data->SetArrayField(TEXT("expressions"), Expressions);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("update"))
				{
					// Recompiles the function and every material using it.
					UMaterialEditingLibrary::UpdateMaterialFunction(Function);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("function"), Function->GetPathName());
					Data->SetStringField(TEXT("message"),
						TEXT("function and every material referencing it recompiled"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Function, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("function"), Function->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditMaterialFunction", "McpLink Edit Material Function"));
				Function->Modify();

				if (Operation == TEXT("add_expression"))
				{
					FString ClassSpec;
					if (!RequireString(Body, TEXT("class"), ClassSpec, Responder,
							TEXT("e.g. FunctionInput, FunctionOutput, Add — material_graph ")
							TEXT("list_expression_classes lists them all")))
					{
						return;
					}
					UClass* Class = FunctionExpressionClass(ClassSpec);
					if (Class == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_class"),
							FString::Printf(
								TEXT("'%s' is not a concrete UMaterialExpression class — see ")
								TEXT("material_graph list_expression_classes"),
								*ClassSpec));
						return;
					}
					UMaterialExpression* Expression =
						UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, Class,
							IntOr(Body, TEXT("x"), 0), IntOr(Body, TEXT("y"), 0));
					if (Expression == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_failed"),
							FString::Printf(TEXT("could not create a %s in this function"), *ClassSpec));
						return;
					}
					FString Name;
					if (Body->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
					{
						if (UMaterialExpressionFunctionInput* Input =
								Cast<UMaterialExpressionFunctionInput>(Expression))
						{
							Input->InputName = FName(*Name);
						}
						else if (UMaterialExpressionFunctionOutput* Output =
									 Cast<UMaterialExpressionFunctionOutput>(Expression))
						{
							Output->OutputName = FName(*Name);
						}
					}
					Responder->Ok(FunctionExpressionToJson(Expression));
					return;
				}

				if (Operation == TEXT("delete_expression"))
				{
					FString Spec;
					if (!RequireString(Body, TEXT("expression"), Spec, Responder,
							TEXT("an expression name or path from `info`")))
					{
						return;
					}
					UMaterialExpression* Expression = FindExpression(Function, Spec);
					if (Expression == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("expression_not_found"),
							FString::Printf(TEXT("no expression '%s' in this function"), *Spec));
						return;
					}
					UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, Expression);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Spec);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("connect"))
				{
					FString FromSpec, ToSpec, FromOutput, ToInput;
					if (!RequireString(Body, TEXT("from"), FromSpec, Responder,
							TEXT("the source expression"))
						|| !RequireString(Body, TEXT("to"), ToSpec, Responder,
							TEXT("the destination expression")))
					{
						return;
					}
					Body->TryGetStringField(TEXT("from_output"), FromOutput);
					Body->TryGetStringField(TEXT("to_input"), ToInput);
					UMaterialExpression* From = FindExpression(Function, FromSpec);
					UMaterialExpression* To = FindExpression(Function, ToSpec);
					if (From == nullptr || To == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("expression_not_found"),
							FString::Printf(TEXT("no expression '%s' in this function"),
								From == nullptr ? *FromSpec : *ToSpec));
						return;
					}
					if (!UMaterialEditingLibrary::ConnectMaterialExpressions(
							From, FromOutput, To, ToInput))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("connect_failed"),
							FString::Printf(
								TEXT("could not connect %s.%s to %s.%s — check the pin names in `info`"),
								*FromSpec, FromOutput.IsEmpty() ? TEXT("(default)") : *FromOutput,
								*ToSpec, ToInput.IsEmpty() ? TEXT("(default)") : *ToInput));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("from"), FromSpec);
					Data->SetStringField(TEXT("to"), ToSpec);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("layout"))
				{
					UMaterialEditingLibrary::LayoutMaterialFunctionExpressions(Function);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("function"), Function->GetPathName());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, info, add_expression, ")
						TEXT("delete_expression, connect, layout, update, or save"),
						*Operation));
			});
	}
}
