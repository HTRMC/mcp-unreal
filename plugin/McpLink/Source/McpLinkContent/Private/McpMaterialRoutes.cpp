// Material and texture routes: create material instances, read and write their
// parameters, and inspect textures.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialEditingLibrary.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

namespace McpLink
{
	namespace
	{
		UMaterialInstanceConstant* InstanceOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("material"), Path, Responder,
				TEXT("a Material Instance asset path, e.g. /Game/Materials/MI_Thing")))
			{
				return nullptr;
			}
			UMaterialInstanceConstant* Instance =
				Cast<UMaterialInstanceConstant>(ResolveObject(Path));
			if (Instance == nullptr && !Path.Contains(TEXT(".")))
			{
				const FString WithAsset =
					FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path));
				Instance = Cast<UMaterialInstanceConstant>(ResolveObject(WithAsset));
			}
			if (Instance == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("instance_not_found"),
					FString::Printf(
						TEXT("no Material Instance at '%s' — create one with operation create_instance"),
						*Path));
			}
			return Instance;
		}

		UMaterialInterface* MaterialInterfaceOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("material"), Path, Responder,
				TEXT("a Material or Material Instance asset path")))
			{
				return nullptr;
			}
			UObject* Object = ResolveObject(Path);
			if (Object == nullptr && !Path.Contains(TEXT(".")))
			{
				Object = ResolveObject(
					FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path)));
			}
			UMaterialInterface* Material = Cast<UMaterialInterface>(Object);
			if (Material == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("material_not_found"),
					FString::Printf(TEXT("no Material or Material Instance at '%s'"), *Path));
			}
			return Material;
		}

		TArray<TSharedPtr<FJsonValue>> NamesToJson(const TArray<FName>& Names)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FName& Name : Names)
			{
				Out.Add(MakeShared<FJsonValueString>(Name.ToString()));
			}
			return Out;
		}
	}

	void RegisterMaterialRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/materials/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
						TEXT("e.g. /Game/Materials/M_Thing")))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMaterial", "McpLink Create Material"));
					UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
					FString Error;
					UObject* Asset = CreateAsset(Path, UMaterial::StaticClass(), Factory, Error);
					if (Asset == nullptr)
					{
						Responder->Error(
							EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Asset->GetPathName());
					Data->SetStringField(TEXT("message"),
						TEXT("empty material created — assign it to a component, or create an instance of it. ")
						TEXT("Build its node graph with material_graph (add_expression, connect_property)."));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_instance"))
				{
					FString Path, ParentPath;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Materials/MI_Thing"))
						|| !RequireString(Body, TEXT("parent"), ParentPath, Responder,
							TEXT("the Material to instance, e.g. /Engine/BasicShapes/BasicShapeMaterial")))
					{
						return;
					}
					UObject* ParentObject = ResolveObject(ParentPath);
					if (ParentObject == nullptr && !ParentPath.Contains(TEXT(".")))
					{
						ParentObject = ResolveObject(FString::Printf(
							TEXT("%s.%s"), *ParentPath, *FPackageName::GetShortName(ParentPath)));
					}
					UMaterialInterface* Parent = Cast<UMaterialInterface>(ParentObject);
					if (Parent == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("parent_not_found"),
							FString::Printf(TEXT("no Material at '%s'"), *ParentPath));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMaterialInstance", "McpLink Create Material Instance"));
					UMaterialInstanceConstantFactoryNew* Factory =
						NewObject<UMaterialInstanceConstantFactoryNew>();
					Factory->InitialParent = Parent;
					FString Error;
					UObject* Asset =
						CreateAsset(Path, UMaterialInstanceConstant::StaticClass(), Factory, Error);
					if (Asset == nullptr)
					{
						Responder->Error(
							EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Asset->GetPathName());
					Data->SetStringField(TEXT("parent"), Parent->GetPathName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_parameters"))
				{
					UMaterialInterface* Material = MaterialInterfaceOrError(Body, Responder);
					if (!Material) { return; }

					TArray<FName> Scalars, Vectors, Textures, Switches;
					UMaterialEditingLibrary::GetScalarParameterNames(Material, Scalars);
					UMaterialEditingLibrary::GetVectorParameterNames(Material, Vectors);
					UMaterialEditingLibrary::GetTextureParameterNames(Material, Textures);
					UMaterialEditingLibrary::GetStaticSwitchParameterNames(Material, Switches);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("material"), Material->GetPathName());
					Data->SetArrayField(TEXT("scalar"), NamesToJson(Scalars));
					Data->SetArrayField(TEXT("vector"), NamesToJson(Vectors));
					Data->SetArrayField(TEXT("texture"), NamesToJson(Textures));
					Data->SetArrayField(TEXT("static_switch"), NamesToJson(Switches));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("get_parameter"))
				{
					UMaterialInterface* Material = MaterialInterfaceOrError(Body, Responder);
					if (!Material) { return; }
					FString ParameterName;
					if (!RequireString(Body, TEXT("parameter"), ParameterName, Responder))
					{
						return;
					}
					const FName Name(*ParameterName);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("parameter"), ParameterName);

					float ScalarValue = 0.0f;
					FLinearColor VectorValue = FLinearColor::Black;
					UTexture* TextureValue = nullptr;
					if (Material->GetScalarParameterValue(Name, ScalarValue))
					{
						Data->SetStringField(TEXT("type"), TEXT("scalar"));
						Data->SetNumberField(TEXT("value"), ScalarValue);
					}
					else if (Material->GetVectorParameterValue(Name, VectorValue))
					{
						Data->SetStringField(TEXT("type"), TEXT("vector"));
						TArray<TSharedPtr<FJsonValue>> Components = {
							MakeShared<FJsonValueNumber>(VectorValue.R),
							MakeShared<FJsonValueNumber>(VectorValue.G),
							MakeShared<FJsonValueNumber>(VectorValue.B),
							MakeShared<FJsonValueNumber>(VectorValue.A)};
						Data->SetArrayField(TEXT("value"), Components);
					}
					else if (Material->GetTextureParameterValue(Name, TextureValue))
					{
						Data->SetStringField(TEXT("type"), TEXT("texture"));
						Data->SetStringField(TEXT("value"),
							TextureValue ? TextureValue->GetPathName() : TEXT(""));
					}
					else
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("parameter_not_found"),
							FString::Printf(
								TEXT("'%s' has no parameter '%s' — use list_parameters"),
								*Material->GetName(), *ParameterName));
						return;
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_parameter"))
				{
					UMaterialInstanceConstant* Instance = InstanceOrError(Body, Responder);
					if (!Instance) { return; }
					FString ParameterName;
					if (!RequireString(Body, TEXT("parameter"), ParameterName, Responder))
					{
						return;
					}
					const FName Name(*ParameterName);

					// Work out which kind of parameter the request is setting.
					double ScalarValue = 0.0;
					const TArray<TSharedPtr<FJsonValue>>* VectorValue = nullptr;
					FString TexturePath;
					FString AppliedType;
					if (Body->TryGetArrayField(TEXT("value"), VectorValue) && VectorValue != nullptr)
					{
						AppliedType = TEXT("vector");
					}
					else if (Body->TryGetStringField(TEXT("texture"), TexturePath))
					{
						AppliedType = TEXT("texture");
					}
					else if (Body->TryGetNumberField(TEXT("value"), ScalarValue))
					{
						AppliedType = TEXT("scalar");
					}
					else
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("pass 'value' as a number (scalar) or [R,G,B,A] array (vector), ")
							TEXT("or 'texture' as an asset path"));
						return;
					}

					// UMaterialEditingLibrary::SetMaterialInstance*ParameterValue always
					// returns false in 5.8 (bResult is never assigned), so the return
					// value can't tell us whether the parameter exists. Check the
					// parameter list first; the setters silently create nothing for
					// unknown names, which would otherwise look like success.
					TArray<FName> KnownNames;
					if (AppliedType == TEXT("vector"))
					{
						UMaterialEditingLibrary::GetVectorParameterNames(Instance, KnownNames);
					}
					else if (AppliedType == TEXT("texture"))
					{
						UMaterialEditingLibrary::GetTextureParameterNames(Instance, KnownNames);
					}
					else
					{
						UMaterialEditingLibrary::GetScalarParameterNames(Instance, KnownNames);
					}
					if (!KnownNames.Contains(Name))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("parameter_not_found"),
							FString::Printf(
								TEXT("'%s' has no %s parameter '%s' — use list_parameters to see what it exposes"),
								*Instance->GetName(), *AppliedType, *ParameterName));
						return;
					}

					UTexture* Texture = nullptr;
					if (AppliedType == TEXT("texture"))
					{
						Texture = Cast<UTexture>(ResolveObject(TexturePath));
						if (Texture == nullptr && !TexturePath.Contains(TEXT(".")))
						{
							Texture = Cast<UTexture>(ResolveObject(FString::Printf(
								TEXT("%s.%s"), *TexturePath, *FPackageName::GetShortName(TexturePath))));
						}
						if (Texture == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("texture_not_found"),
								FString::Printf(TEXT("no Texture at '%s'"), *TexturePath));
							return;
						}
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetMaterialParameter", "McpLink Set Material Parameter"));
					Instance->Modify();

					if (AppliedType == TEXT("vector"))
					{
						double Channels[4] = {0.0, 0.0, 0.0, 1.0};
						for (int32 i = 0; i < VectorValue->Num() && i < 4; ++i)
						{
							(*VectorValue)[i]->TryGetNumber(Channels[i]);
						}
						UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(
							Instance, Name, FLinearColor(Channels[0], Channels[1], Channels[2], Channels[3]));
					}
					else if (AppliedType == TEXT("texture"))
					{
						UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
							Instance, Name, Texture);
					}
					else
					{
						UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(
							Instance, Name, static_cast<float>(ScalarValue));
					}
					UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("material"), Instance->GetPathName());
					Data->SetStringField(TEXT("parameter"), ParameterName);
					Data->SetStringField(TEXT("type"), AppliedType);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					UMaterialInterface* Material = MaterialInterfaceOrError(Body, Responder);
					if (!Material) { return; }
					FString Filename, Error;
					if (!SaveAsset(Material, Filename, Error))
					{
						Responder->Error(
							EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("saved"), true);
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, create_instance, list_parameters, ")
						TEXT("get_parameter, set_parameter, or save"),
						*Operation));
			});

		Core.RegisterRoute(TEXT("/api/textures/info"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Path;
				if (!RequireString(Body, TEXT("texture"), Path, Responder,
					TEXT("a Texture asset path")))
				{
					return;
				}
				UObject* Object = ResolveObject(Path);
				if (Object == nullptr && !Path.Contains(TEXT(".")))
				{
					Object = ResolveObject(
						FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path)));
				}
				UTexture2D* Texture = Cast<UTexture2D>(Object);
				if (Texture == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("texture_not_found"),
						FString::Printf(TEXT("no Texture2D at '%s'"), *Path));
					return;
				}
				// The platform (streamed) size is 0 until the texture resource
				// exists, which never happens under -nullrhi; the editor-only
				// source data is always available, so fall back to it.
				int64 Width = Texture->GetSizeX();
				int64 Height = Texture->GetSizeY();
				const bool bFromSource = Width == 0 || Height == 0;
				if (bFromSource)
				{
					Width = Texture->Source.GetSizeX();
					Height = Texture->Source.GetSizeY();
				}

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("path"), Texture->GetPathName());
				Data->SetNumberField(TEXT("width"), static_cast<double>(Width));
				Data->SetNumberField(TEXT("height"), static_cast<double>(Height));
				Data->SetStringField(TEXT("pixel_format"),
					GPixelFormats[Texture->GetPixelFormat()].Name);
				Data->SetStringField(TEXT("source_format"),
					StaticEnum<ETextureSourceFormat>()->GetNameStringByValue(
						static_cast<int64>(Texture->Source.GetFormat())));
				Data->SetBoolField(TEXT("size_from_source"), bFromSource);
				Data->SetBoolField(TEXT("srgb"), Texture->SRGB);
				Data->SetNumberField(TEXT("lod_bias"), Texture->LODBias);
				Data->SetNumberField(TEXT("num_mips"), Texture->Source.GetNumMips());
				Responder->Ok(Data);
			});
	}
}
