// The reference graph: what an asset needs, what needs it, and how big the
// whole tree is — the Reference Viewer and Size Map, as data.
//
// get_asset_info already reports one asset's direct dependencies. The two
// questions it cannot answer are the ones that matter before a delete ("who
// would break?") and before a cook ("what does this actually cost?").

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResponder.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

namespace McpLink
{
	namespace References
	{
		IAssetRegistry& Registry()
		{
			return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		}

		/// "/Game/Foo" or "/Game/Foo.Foo" -> the package name the registry uses.
		FName PackageNameOf(const FString& Spec)
		{
			return FName(*(Spec.Contains(TEXT(".")) ? FPackageName::ObjectPathToPackageName(Spec) : Spec));
		}

		/// On-disk size of a package, or 0 when it has no file (an unsaved or
		/// script package).
		int64 PackageSize(FName PackageName)
		{
			FString Filename;
			if (!FPackageName::DoesPackageExist(PackageName.ToString(), &Filename))
			{
				return 0;
			}
			return IFileManager::Get().FileSize(*Filename);
		}
	}

	using namespace References;

	void RegisterReferenceRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/workflow/references"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				FString Spec;
				if (!RequireString(Body, TEXT("asset"), Spec, Responder,
						TEXT("an asset or package path, e.g. /Game/Meshes/SM_Rock")))
				{
					return;
				}
				const FName PackageName = PackageNameOf(Spec);
				if (!FPackageName::DoesPackageExist(PackageName.ToString()))
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
						FString::Printf(TEXT("no package '%s' on disk"), *PackageName.ToString()));
					return;
				}

				const bool bHard = BoolOr(Body, TEXT("hard_only"), true);
				const UE::AssetRegistry::EDependencyQuery Query =
					bHard ? UE::AssetRegistry::EDependencyQuery::Hard
						  : UE::AssetRegistry::EDependencyQuery::NoRequirements;
				const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_results"), 200), 1, 5000);

				if (Operation == TEXT("referencers") || Operation == TEXT("dependencies"))
				{
					const bool bReferencers = Operation == TEXT("referencers");
					TArray<FName> Found;
					if (bReferencers)
					{
						Registry().GetReferencers(
							PackageName, Found, UE::AssetRegistry::EDependencyCategory::Package, Query);
					}
					else
					{
						Registry().GetDependencies(
							PackageName, Found, UE::AssetRegistry::EDependencyCategory::Package, Query);
					}
					Found.Sort(FNameLexicalLess());

					TArray<TSharedPtr<FJsonValue>> Packages;
					for (const FName& Name : Found)
					{
						if (Packages.Num() >= Max)
						{
							break;
						}
						Packages.Add(MakeShared<FJsonValueString>(Name.ToString()));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), PackageName.ToString());
					Data->SetBoolField(TEXT("hard_only"), bHard);
					Data->SetNumberField(TEXT("total"), Found.Num());
					Data->SetArrayField(bReferencers ? TEXT("referencers") : TEXT("dependencies"), Packages);
					if (bReferencers && Found.IsEmpty())
					{
						Data->SetStringField(TEXT("message"),
							TEXT("nothing references this package — deleting it breaks nothing"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("size_map"))
				{
					// Walk the whole dependency tree once, so shared assets are
					// counted a single time — which is the point of a size map.
					TSet<FName> Visited;
					TArray<FName> Queue{PackageName};
					const int32 Depth = FMath::Clamp(IntOr(Body, TEXT("max_depth"), 8), 1, 32);
					int32 Level = 0;
					while (!Queue.IsEmpty() && Level < Depth)
					{
						TArray<FName> Next;
						for (const FName& Name : Queue)
						{
							bool bAlready = false;
							Visited.Add(Name, &bAlready);
							if (bAlready)
							{
								continue;
							}
							TArray<FName> Dependencies;
							Registry().GetDependencies(Name, Dependencies,
								UE::AssetRegistry::EDependencyCategory::Package, Query);
							Next.Append(Dependencies);
						}
						Queue = MoveTemp(Next);
						++Level;
					}

					struct FEntry
					{
						FName Package;
						int64 Size;
					};
					TArray<FEntry> Entries;
					int64 Total = 0;
					for (const FName& Name : Visited)
					{
						const int64 Size = PackageSize(Name);
						Total += Size;
						Entries.Add({Name, Size});
					}
					Entries.Sort([](const FEntry& A, const FEntry& B) { return A.Size > B.Size; });

					TArray<TSharedPtr<FJsonValue>> Largest;
					for (const FEntry& Entry : Entries)
					{
						if (Largest.Num() >= Max)
						{
							break;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("package"), Entry.Package.ToString());
						Item->SetNumberField(TEXT("size_bytes"), static_cast<double>(Entry.Size));
						Largest.Add(MakeShared<FJsonValueObject>(Item));
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), PackageName.ToString());
					Data->SetNumberField(TEXT("packages"), Visited.Num());
					Data->SetNumberField(TEXT("total_bytes"), static_cast<double>(Total));
					Data->SetNumberField(TEXT("depth_walked"), Level);
					Data->SetArrayField(TEXT("largest"), Largest);
					Data->SetStringField(TEXT("message"),
						TEXT("on-disk .uasset sizes, deduplicated across the tree — an editor-side ")
						TEXT("estimate, not the cooked size"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use referencers, dependencies, or size_map"),
						*Operation));
			});
	}
}
