// Shared helpers for routes that create, save or validate assets.
#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"

class FJsonObject;
class UClass;
class UFactory;
class UObject;

namespace McpLink
{
	class FMcpResponder;

	/// Create a new asset of `Class` at `PackagePath` using the asset tools, so
	/// the asset registry and content browser pick it up. Returns nullptr and
	/// fills OutError when the path is taken or invalid.
	MCPLINKCORE_API UObject* CreateAsset(
		const FString& PackagePath,
		UClass* Class,
		UFactory* Factory,
		FString& OutError);

	/// Save the package owning `Asset` to its .uasset file.
	MCPLINKCORE_API bool SaveAsset(UObject* Asset, FString& OutFilename, FString& OutError);

	/// Common "'field' is required" 400 response. Returns false when missing.
	MCPLINKCORE_API bool RequireString(
		const TSharedRef<FJsonObject>& Body,
		const TCHAR* Field,
		FString& OutValue,
		const TSharedRef<FMcpResponder>& Responder,
		const TCHAR* Hint = nullptr);
}
