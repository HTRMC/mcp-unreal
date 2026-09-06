// Geometry Script as an API: dynamic meshes that live in the transient
// package, every UGeometryScriptLibrary_* function callable on them by name
// through reflection, and the way out into a Static Mesh asset.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/CreateNewAssetUtilityFunctions.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpReflection.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace GeometryScript
	{
		const TCHAR* LibraryPrefix = TEXT("GeometryScriptLibrary_");

		UDynamicMesh* MeshOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("mesh"), Path, Responder, TEXT("a dynamic mesh path from new_mesh")))
			{
				return nullptr;
			}
			UDynamicMesh* Mesh = Cast<UDynamicMesh>(ResolveObject(Path));
			if (!IsValid(Mesh))
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("mesh_not_found"),
					FString::Printf(TEXT("no dynamic mesh at '%s' — new_mesh makes one"), *Path));
			}
			return Mesh;
		}

		TSharedRef<FJsonObject> MeshJson(UDynamicMesh& Mesh)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("mesh"), Mesh.GetPathName());
			Mesh.ProcessMesh([&Data](const UE::Geometry::FDynamicMesh3& M)
			{
				Data->SetNumberField(TEXT("vertices"), M.VertexCount());
				Data->SetNumberField(TEXT("triangles"), M.TriangleCount());
				Data->SetBoolField(TEXT("has_attributes"), M.HasAttributes());
				if (M.VertexCount() > 0)
				{
					const UE::Geometry::FAxisAlignedBox3d Bounds = M.GetBounds();
					const TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
					BoundsJson->SetArrayField(TEXT("min"), VectorToJson(FVector(Bounds.Min)));
					BoundsJson->SetArrayField(TEXT("max"), VectorToJson(FVector(Bounds.Max)));
					Data->SetObjectField(TEXT("bounds"), BoundsJson);
				}
			});
			return Data;
		}

		/// Every Geometry Script library class, editor ones included.
		TArray<UClass*> LibraryClasses()
		{
			TArray<UClass*> Out;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->GetName().StartsWith(LibraryPrefix) && !It->HasAnyClassFlags(CLASS_Abstract))
				{
					Out.Add(*It);
				}
			}
			Out.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
			return Out;
		}

		FString LibraryShortName(const UClass& Class)
		{
			FString Name = Class.GetName();
			Name.RemoveFromStart(LibraryPrefix);
			return Name;
		}

		/// The function named — in the given library, or in whichever library
		/// has it when it is unambiguous. Null after responding.
		UFunction* FunctionOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder, UClass*& OutClass)
		{
			FString FunctionName, Library;
			if (!RequireString(Body, TEXT("function"), FunctionName, Responder,
					TEXT("a Geometry Script function, e.g. AppendBox (list_functions finds them)")))
			{
				return nullptr;
			}
			Body->TryGetStringField(TEXT("library"), Library);
			Library.RemoveFromStart(LibraryPrefix);
			TArray<TPair<UClass*, UFunction*>> Matches;
			for (UClass* Class : LibraryClasses())
			{
				if (!Library.IsEmpty() && LibraryShortName(*Class) != Library)
				{
					continue;
				}
				if (UFunction* Function = Class->FindFunctionByName(FName(*FunctionName)))
				{
					Matches.Emplace(Class, Function);
				}
			}
			if (Matches.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("function_not_found"),
					Library.IsEmpty()
						? FString::Printf(TEXT("no Geometry Script library has a function '%s' — list_functions with 'contains' searches them"), *FunctionName)
						: FString::Printf(TEXT("library '%s' has no function '%s'"), *Library, *FunctionName));
				return nullptr;
			}
			if (Matches.Num() > 1)
			{
				TArray<FString> Names;
				for (const TPair<UClass*, UFunction*>& Match : Matches)
				{
					Names.Add(LibraryShortName(*Match.Key));
				}
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("ambiguous_function"),
					FString::Printf(TEXT("'%s' exists in several libraries (%s) — pass 'library'"),
						*FunctionName, *FString::Join(Names, TEXT(", "))));
				return nullptr;
			}
			OutClass = Matches[0].Key;
			return Matches[0].Value;
		}

		/// Static mesh at a package path, or a new one there.
		UStaticMesh* StaticMeshAtOrError(const FString& Path, bool bCreate, const TSharedRef<FMcpResponder>& Responder, bool& bOutCreated)
		{
			bOutCreated = false;
			if (UStaticMesh* Existing = Cast<UStaticMesh>(ResolveAsset(Path)))
			{
				return Existing;
			}
			if (!bCreate)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("static_mesh_not_found"),
					FString::Printf(TEXT("no Static Mesh at '%s'"), *Path));
				return nullptr;
			}
			if (!FPackageName::IsValidLongPackageName(Path))
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_path"),
					FString::Printf(TEXT("'%s' is not a package path — use /Game/Folder/Name"), *Path));
				return nullptr;
			}
			if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
					FString::Printf(TEXT("'%s' exists but is not a Static Mesh"), *Path));
				return nullptr;
			}
			bOutCreated = true;
			return nullptr;
		}
	}

	void RegisterGeometryScriptRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace GeometryScript;

		Core.RegisterRoute(TEXT("/api/geometry/script"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("new_mesh"))
				{
					FString Name;
					Body->TryGetStringField(TEXT("name"), Name);
					if (Name.IsEmpty())
					{
						Name = TEXT("McpDynamicMesh");
					}
					// Rooted so it outlives garbage collection between calls;
					// release drops it.
					// The name as given while it is free; a suffix only on a clash.
					FName MeshName(*Name);
					if (StaticFindObjectFast(nullptr, GetTransientPackage(), MeshName) != nullptr)
					{
						MeshName = MakeUniqueObjectName(GetTransientPackage(), UDynamicMesh::StaticClass(), MeshName);
					}
					UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage(), MeshName);
					Mesh->AddToRoot();
					const TSharedRef<FJsonObject> Data = MeshJson(*Mesh);
					Data->SetStringField(TEXT("message"),
						TEXT("empty — call AppendBox / AppendSphere / CopyMeshFromStaticMesh and the rest on it, ")
						TEXT("then to_static_mesh; release when done"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_functions"))
				{
					FString Contains, Library;
					Body->TryGetStringField(TEXT("contains"), Contains);
					Body->TryGetStringField(TEXT("library"), Library);
					Library.RemoveFromStart(LibraryPrefix);
					const bool bSignatures = BoolOr(Body, TEXT("signatures"), false);
					const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_results"), 200), 1, 2000);
					TArray<TSharedPtr<FJsonValue>> Functions;
					TArray<TSharedPtr<FJsonValue>> Libraries;
					int32 Total = 0;
					for (UClass* Class : LibraryClasses())
					{
						const FString Short = LibraryShortName(*Class);
						Libraries.Add(MakeShared<FJsonValueString>(Short));
						if (!Library.IsEmpty() && Short != Library)
						{
							continue;
						}
						for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
						{
							if (!It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)
								|| (!Contains.IsEmpty() && !It->GetName().Contains(Contains)))
							{
								continue;
							}
							++Total;
							if (Functions.Num() >= Max)
							{
								continue;
							}
							if (bSignatures)
							{
								const TSharedRef<FJsonObject> Entry = FunctionSignatureJson(**It);
								Entry->SetStringField(TEXT("library"), Short);
								Functions.Add(MakeShared<FJsonValueObject>(Entry));
							}
							else
							{
								Functions.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"), *Short, *It->GetName())));
							}
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("libraries"), Libraries);
					Data->SetNumberField(TEXT("total"), Total);
					Data->SetArrayField(TEXT("functions"), Functions);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("signature"))
				{
					UClass* Class = nullptr;
					UFunction* Function = FunctionOrError(Body, Responder, Class);
					if (Function == nullptr)
					{
						return;
					}
					const TSharedRef<FJsonObject> Data = FunctionSignatureJson(*Function);
					Data->SetStringField(TEXT("library"), LibraryShortName(*Class));
					Responder->Ok(Data);
					return;
				}

				UDynamicMesh* Mesh = MeshOrError(Body, Responder);
				if (Mesh == nullptr)
				{
					return;
				}

				if (Operation == TEXT("mesh_info"))
				{
					const TSharedRef<FJsonObject> Data = MeshJson(*Mesh);
					Data->SetStringField(TEXT("summary"), UGeometryScriptLibrary_MeshQueryFunctions::GetMeshInfoString(Mesh));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("release"))
				{
					const FString Path = Mesh->GetPathName();
					Mesh->RemoveFromRoot();
					// Free the name for the next new_mesh before garbage
					// collection gets round to the object.
					Mesh->Rename(*MakeUniqueObjectName(GetTransientPackage(), UDynamicMesh::StaticClass(),
						TEXT("McpReleasedMesh")).ToString(), GetTransientPackage(), REN_NonTransactional | REN_DontCreateRedirectors);
					Mesh->MarkAsGarbage();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("released"), Path);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("call"))
				{
					UClass* Class = nullptr;
					UFunction* Function = FunctionOrError(Body, Responder, Class);
					if (Function == nullptr)
					{
						return;
					}
					const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
					Body->TryGetObjectField(TEXT("args"), ArgsPtr);
					const TSharedRef<FJsonObject> Args = ArgsPtr != nullptr && ArgsPtr->IsValid()
						? ArgsPtr->ToSharedRef() : MakeShared<FJsonObject>();
					// The mesh fills the first UDynamicMesh parameter that was
					// not given — TargetMesh on nearly every function.
					for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
					{
						const FObjectProperty* ObjectParam = CastField<FObjectProperty>(*It);
						if (ObjectParam != nullptr && !It->HasAnyPropertyFlags(CPF_ReturnParm)
							&& ObjectParam->PropertyClass == UDynamicMesh::StaticClass()
							&& !Args->HasField(It->GetName()))
						{
							Args->SetStringField(It->GetName(), Mesh->GetPathName());
							break;
						}
					}
					const TSharedRef<FJsonObject> Outputs = MakeShared<FJsonObject>();
					FString Error;
					if (!CallFunctionFromJson(Class->GetDefaultObject(), Function, Args, Outputs, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_argument"),
							Error + TEXT(" — signature reports the parameters"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MeshJson(*Mesh);
					Data->SetStringField(TEXT("function"), FString::Printf(TEXT("%s.%s"), *LibraryShortName(*Class), *Function->GetName()));
					Data->SetObjectField(TEXT("outputs"), Outputs);
					FString Outcome;
					if (Outputs->TryGetStringField(TEXT("Outcome"), Outcome))
					{
						Data->SetBoolField(TEXT("succeeded"), Outcome == TEXT("Success"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("to_static_mesh"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("a Static Mesh asset path — existing to overwrite, or new")))
					{
						return;
					}
					bool bCreated = false;
					UStaticMesh* StaticMesh = StaticMeshAtOrError(Path, true, Responder, bCreated);
					if (StaticMesh == nullptr && !bCreated)
					{
						return;
					}
					EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
					if (bCreated)
					{
						FGeometryScriptCreateNewStaticMeshAssetOptions Options;
						Options.bEnableRecomputeNormals = BoolOr(Body, TEXT("recompute_normals"), false);
						Options.bEnableRecomputeTangents = BoolOr(Body, TEXT("recompute_tangents"), false);
						Options.bEnableNanite = BoolOr(Body, TEXT("nanite"), false);
						StaticMesh = UGeometryScriptLibrary_CreateNewAssetFunctions::CreateNewStaticMeshAssetFromMesh(
							Mesh, Path, Options, Outcome, nullptr);
					}
					else
					{
						FGeometryScriptCopyMeshToAssetOptions Options;
						Options.bEnableRecomputeNormals = BoolOr(Body, TEXT("recompute_normals"), false);
						Options.bEnableRecomputeTangents = BoolOr(Body, TEXT("recompute_tangents"), false);
						Options.bReplaceMaterials = BoolOr(Body, TEXT("replace_materials"), false);
						FGeometryScriptMeshWriteLOD Lod;
						Lod.LODIndex = IntOr(Body, TEXT("lod"), 0);
						StaticMesh->Modify();
						UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
							Mesh, StaticMesh, Options, Lod, Outcome, true, nullptr);
					}
					if (Outcome != EGeometryScriptOutcomePins::Success || StaticMesh == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("write_failed"),
							TEXT("Geometry Script could not write the Static Mesh — is the dynamic mesh empty?"));
						return;
					}
					StaticMesh->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MeshJson(*Mesh);
					Data->SetStringField(TEXT("static_mesh"), StaticMesh->GetPathName());
					Data->SetBoolField(TEXT("created"), bCreated);
					Data->SetNumberField(TEXT("lods"), StaticMesh->GetNumSourceModels());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("from_static_mesh"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("static_mesh"), Path, Responder, TEXT("a Static Mesh asset path")))
					{
						return;
					}
					bool bCreated = false;
					UStaticMesh* StaticMesh = StaticMeshAtOrError(Path, false, Responder, bCreated);
					if (StaticMesh == nullptr)
					{
						return;
					}
					FGeometryScriptCopyMeshFromAssetOptions Options;
					FGeometryScriptMeshReadLOD Lod;
					Lod.LODIndex = IntOr(Body, TEXT("lod"), 0);
					EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
					UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(StaticMesh, Mesh, Options, Lod, Outcome, nullptr);
					if (Outcome != EGeometryScriptOutcomePins::Success)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("read_failed"),
							FString::Printf(TEXT("Geometry Script could not read LOD %d of '%s'"), Lod.LODIndex, *Path));
						return;
					}
					const TSharedRef<FJsonObject> Data = MeshJson(*Mesh);
					Data->SetStringField(TEXT("static_mesh"), StaticMesh->GetPathName());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected new_mesh, list_functions, signature, ")
						TEXT("call, mesh_info, to_static_mesh, from_static_mesh or release"), *Operation));
			});
	}
}
