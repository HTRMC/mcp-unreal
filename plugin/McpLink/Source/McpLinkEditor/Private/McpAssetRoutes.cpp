#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"

namespace McpLink
{
	namespace
	{
		IAssetRegistry& GetAssetRegistry()
		{
			return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		}

		TSharedRef<FJsonObject> AssetToJson(const FAssetData& Asset)
		{
			const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			Obj->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
			Obj->SetStringField(TEXT("package"), Asset.PackageName.ToString());
			Obj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
			return Obj;
		}
	}

	void RegisterAssetRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/assets/search"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FARFilter Filter;
				Filter.bRecursivePaths = true;
				Filter.bRecursiveClasses = true;

				FString PathPrefix = TEXT("/Game");
				Body->TryGetStringField(TEXT("path_prefix"), PathPrefix);
				Filter.PackagePaths.Add(FName(*PathPrefix));

				FString ClassSpec;
				if (Body->TryGetStringField(TEXT("class"), ClassSpec) && !ClassSpec.IsEmpty())
				{
					UClass* Class = ResolveClass(ClassSpec);
					if (Class == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_class"),
							FString::Printf(TEXT("class '%s' not found"), *ClassSpec));
						return;
					}
					Filter.ClassPaths.Add(Class->GetClassPathName());
				}

				FString NameContains;
				Body->TryGetStringField(TEXT("name_contains"), NameContains);
				const int32 MaxResults = FMath::Clamp(
					Body->HasTypedField<EJson::Number>(TEXT("max_results"))
						? static_cast<int32>(Body->GetNumberField(TEXT("max_results")))
						: 50,
					1, 500);

				TArray<FAssetData> Assets;
				GetAssetRegistry().GetAssets(Filter, Assets);

				TArray<TSharedPtr<FJsonValue>> Results;
				int32 Total = 0;
				for (const FAssetData& Asset : Assets)
				{
					if (!NameContains.IsEmpty()
						&& !Asset.AssetName.ToString().Contains(NameContains))
					{
						continue;
					}
					++Total;
					if (Results.Num() < MaxResults)
					{
						Results.Add(MakeShared<FJsonValueObject>(AssetToJson(Asset)));
					}
				}

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetNumberField(TEXT("total"), Total);
				Data->SetArrayField(TEXT("assets"), Results);
				Responder->Ok(Data);
			});

		Core.RegisterRoute(TEXT("/api/assets/info"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Path;
				if (!Body->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
						TEXT("'path' is required (e.g. /Game/Meshes/SM_Rock or /Game/Meshes/SM_Rock.SM_Rock)"));
					return;
				}
				if (!Path.Contains(TEXT(".")))
				{
					Path = FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path));
				}

				IAssetRegistry& Registry = GetAssetRegistry();
				const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
				if (!Asset.IsValid())
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
						FString::Printf(TEXT("no asset at '%s' — use /api/assets/search to discover"), *Path));
					return;
				}

				const TSharedRef<FJsonObject> Data = AssetToJson(Asset);

				TArray<FName> Dependencies;
				Registry.GetDependencies(Asset.PackageName, Dependencies);
				TArray<TSharedPtr<FJsonValue>> DependencyValues;
				for (const FName& Dep : Dependencies)
				{
					DependencyValues.Add(MakeShared<FJsonValueString>(Dep.ToString()));
				}
				Data->SetArrayField(TEXT("dependencies"), DependencyValues);

				TArray<FName> Referencers;
				Registry.GetReferencers(Asset.PackageName, Referencers);
				TArray<TSharedPtr<FJsonValue>> ReferencerValues;
				for (const FName& Ref : Referencers)
				{
					ReferencerValues.Add(MakeShared<FJsonValueString>(Ref.ToString()));
				}
				Data->SetArrayField(TEXT("referencers"), ReferencerValues);

				Responder->Ok(Data);
			});
	}
}
