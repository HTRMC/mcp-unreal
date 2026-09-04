#include "McpAssetUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Factories/Factory.h"
#include "HttpServerConstants.h"
#include "IAssetTools.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace McpLink
{
	UObject* CreateAsset(
		const FString& PackagePath, UClass* Class, UFactory* Factory, FString& OutError)
	{
		if (!PackagePath.StartsWith(TEXT("/")))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a package path — use /Game/Folder/AssetName"), *PackagePath);
			return nullptr;
		}
		if (FPackageName::DoesPackageExist(PackagePath) || FindPackage(nullptr, *PackagePath) != nullptr)
		{
			OutError = FString::Printf(TEXT("an asset already exists at '%s'"), *PackagePath);
			return nullptr;
		}

		const FString AssetName = FPackageName::GetShortName(PackagePath);
		const FString FolderPath = FPackageName::GetLongPackagePath(PackagePath);
		IAssetTools& AssetTools =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* Asset = AssetTools.CreateAsset(AssetName, FolderPath, Class, Factory);
		if (Asset == nullptr)
		{
			OutError = FString::Printf(
				TEXT("could not create a %s asset at '%s'"), *Class->GetName(), *PackagePath);
		}
		return Asset;
	}

	bool SaveAsset(UObject* Asset, FString& OutFilename, FString& OutError)
	{
		if (Asset == nullptr)
		{
			OutError = TEXT("no asset to save");
			return false;
		}
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		OutFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, nullptr, *OutFilename, SaveArgs))
		{
			OutError = FString::Printf(TEXT("failed to write '%s'"), *OutFilename);
			return false;
		}
		return true;
	}

	bool RequireString(
		const TSharedRef<FJsonObject>& Body,
		const TCHAR* Field,
		FString& OutValue,
		const TSharedRef<FMcpResponder>& Responder,
		const TCHAR* Hint)
	{
		if (Body->TryGetStringField(Field, OutValue) && !OutValue.IsEmpty())
		{
			return true;
		}
		Responder->Error(
			EHttpServerResponseCodes::BadRequest,
			TEXT("missing_field"),
			Hint != nullptr
				? FString::Printf(TEXT("'%s' is required (%s)"), Field, Hint)
				: FString::Printf(TEXT("'%s' is required"), Field));
		return false;
	}
}
