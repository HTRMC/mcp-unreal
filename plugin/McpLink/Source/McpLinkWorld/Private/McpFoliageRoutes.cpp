// Foliage: the instanced-static-mesh scatter the Foliage editor mode paints.
//
// Foliage type settings (density, scale range, alignment, collision) are plain
// UPROPERTYs, so they are edited with set_property on the `type` path each
// operation reports — this route owns the types and their instances.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
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
		/// Foliage types are addressed by the name they report, which is the
		/// source mesh's name for a mesh type and the object name otherwise.
		FString TypeName(const UFoliageType& Type)
		{
			if (const UFoliageType_InstancedStaticMesh* Mesh = Cast<UFoliageType_InstancedStaticMesh>(&Type))
			{
				if (Mesh->GetStaticMesh() != nullptr)
				{
					return Mesh->GetStaticMesh()->GetName();
				}
			}
			return Type.GetName();
		}

		TSharedRef<FJsonObject> TypeToJson(UFoliageType& Type, const FFoliageInfo& Info)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("name"), TypeName(Type));
			Json->SetStringField(TEXT("type"), Type.GetPathName());
			Json->SetStringField(TEXT("class"), Type.GetClass()->GetName());
			Json->SetNumberField(TEXT("instance_count"), Info.Instances.Num());
			if (const UFoliageType_InstancedStaticMesh* Mesh = Cast<UFoliageType_InstancedStaticMesh>(&Type))
			{
				if (Mesh->GetStaticMesh() != nullptr)
				{
					Json->SetStringField(TEXT("static_mesh"), Mesh->GetStaticMesh()->GetPathName());
				}
			}
			return Json;
		}

		/// The foliage type named by `type`, by reported name or object path.
		UFoliageType* FindType(AInstancedFoliageActor& Actor, const FString& Spec, FFoliageInfo*& OutInfo)
		{
			OutInfo = nullptr;
			for (const auto& Pair : Actor.GetFoliageInfos())
			{
				UFoliageType* Type = Pair.Key;
				if (Type == nullptr)
				{
					continue;
				}
				if (TypeName(*Type) == Spec || Type->GetPathName() == Spec || Type->GetName() == Spec)
				{
					OutInfo = Actor.FindInfo(Type);
					return Type;
				}
			}
			return nullptr;
		}

		FString KnownTypes(AInstancedFoliageActor& Actor)
		{
			TArray<FString> Names;
			for (const auto& Pair : Actor.GetFoliageInfos())
			{
				if (Pair.Key != nullptr) { Names.Add(TypeName(*Pair.Key)); }
			}
			return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : FString(TEXT("none yet"));
		}

		/// Drop a point onto whatever it lands on; false when nothing is under it.
		bool TraceDown(UWorld& World, const FVector& From, double Distance, FHitResult& OutHit)
		{
			FCollisionQueryParams Params(SCENE_QUERY_STAT(McpFoliageScatter), /*bTraceComplex=*/true);
			return World.LineTraceSingleByChannel(
				OutHit, From, From - FVector(0.0, 0.0, Distance), ECC_WorldStatic, Params);
		}
	}

	void RegisterFoliageRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/world/foliage"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);
				UWorld* World = ResolveWorld(Body);
				if (World == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
						TEXT("no world — open a level or start PIE"));
					return;
				}

				// Only the operations that add something should create the actor.
				const bool bCreate = Operation == TEXT("add_type") || Operation == TEXT("add_instances");
				// The level hint is not optional: Get routes through the actor
				// partition subsystem, which asserts on a null level in the
				// level-partition path (a plain, non-World-Partition map).
				AInstancedFoliageActor* Actor =
					AInstancedFoliageActor::Get(World, bCreate, World->GetCurrentLevel());
				if (Actor == nullptr)
				{
					if (Operation == TEXT("list_types"))
					{
						const TSharedRef<FJsonObject> Empty = MakeShared<FJsonObject>();
						Empty->SetArrayField(TEXT("types"), {});
						Responder->Ok(Empty);
						return;
					}
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("no_foliage"),
						TEXT("this level has no foliage yet — add_type makes the first type"));
					return;
				}

				if (Operation == TEXT("list_types"))
				{
					TArray<TSharedPtr<FJsonValue>> Types;
					for (const auto& Pair : Actor->GetFoliageInfos())
					{
						if (Pair.Key != nullptr)
						{
							Types.Add(MakeShared<FJsonValueObject>(TypeToJson(*Pair.Key, *Pair.Value)));
						}
					}
					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("foliage_actor"), Actor->GetPathName());
					Result->SetArrayField(TEXT("types"), Types);
					Responder->Ok(Result);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditFoliage", "McpLink Edit Foliage"),
					ShouldTransact(Actor));

				if (Operation == TEXT("add_type"))
				{
					FString MeshPath;
					if (!RequireString(Body, TEXT("static_mesh"), MeshPath, Responder,
						TEXT("a Static Mesh asset path to scatter, e.g. /Engine/BasicShapes/Cone")))
					{
						return;
					}
					UObject* Object = ResolveObject(MeshPath);
					if (Object == nullptr && !MeshPath.Contains(TEXT(".")))
					{
						Object = ResolveObject(FString::Printf(
							TEXT("%s.%s"), *MeshPath, *FPackageName::GetShortName(MeshPath)));
					}
					UStaticMesh* Mesh = Cast<UStaticMesh>(Object);
					if (Mesh == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("mesh_not_found"),
							FString::Printf(TEXT("no Static Mesh at '%s'"), *MeshPath));
						return;
					}

					Actor->Modify();
					UFoliageType* Type = nullptr;
					FFoliageInfo* Info = Actor->AddMesh(Mesh, &Type);
					if (Info == nullptr || Type == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_type_failed"),
							FString::Printf(TEXT("could not make a foliage type for '%s'"), *Mesh->GetName()));
						return;
					}
					Responder->Ok(TypeToJson(*Type, *Info));
					return;
				}

				FString TypeSpec;
				if (!RequireString(Body, TEXT("type"), TypeSpec, Responder,
					TEXT("a foliage type from list_types")))
				{
					return;
				}
				FFoliageInfo* Info = nullptr;
				UFoliageType* Type = FindType(*Actor, TypeSpec, Info);
				if (Type == nullptr || Info == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("type_not_found"),
						FString::Printf(TEXT("no foliage type '%s' — this level has: %s"),
							*TypeSpec, *KnownTypes(*Actor)));
					return;
				}

				if (Operation == TEXT("remove_type"))
				{
					Actor->Modify();
					UFoliageType* ToRemove = Type;
					Actor->RemoveFoliageType(&ToRemove, 1);
					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("removed"), TypeSpec);
					Responder->Ok(Result);
					return;
				}

				if (Operation == TEXT("list_instances"))
				{
					double MaxResults = 100.0;
					Body->TryGetNumberField(TEXT("max_results"), MaxResults);
					TArray<TSharedPtr<FJsonValue>> Instances;
					for (int32 Index = 0;
						Index < Info->Instances.Num() && Instances.Num() < static_cast<int32>(MaxResults);
						++Index)
					{
						const FFoliageInstance& Instance = Info->Instances[Index];
						const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
						Json->SetNumberField(TEXT("index"), Index);
						Json->SetArrayField(TEXT("location"), VectorToJson(Instance.Location));
						Json->SetArrayField(TEXT("rotation"), RotatorToJson(Instance.Rotation));
						Json->SetArrayField(TEXT("scale"), VectorToJson(FVector(Instance.DrawScale3D)));
						Instances.Add(MakeShared<FJsonValueObject>(Json));
					}
					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("type"), TypeName(*Type));
					Result->SetNumberField(TEXT("instance_count"), Info->Instances.Num());
					Result->SetArrayField(TEXT("instances"), Instances);
					Responder->Ok(Result);
					return;
				}

				if (Operation == TEXT("add_instances"))
				{
					TArray<FFoliageInstance> Built;
					const TArray<TSharedPtr<FJsonValue>>* Transforms = nullptr;

					if (Body->TryGetArrayField(TEXT("instances"), Transforms))
					{
						for (const TSharedPtr<FJsonValue>& Entry : *Transforms)
						{
							const TSharedPtr<FJsonObject> Object = Entry.IsValid() ? Entry->AsObject() : nullptr;
							if (!Object.IsValid())
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_instance"),
									TEXT("each entry of 'instances' is an object with location, and "
										 "optionally rotation and scale"));
								return;
							}
							const TSharedRef<FJsonObject> Ref = Object.ToSharedRef();
							FFoliageInstance Instance;
							if (!GetVector(Ref, TEXT("location"), Instance.Location))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_instance"),
									TEXT("every instance needs a 'location' [X, Y, Z]"));
								return;
							}
							GetRotator(Ref, TEXT("rotation"), Instance.Rotation);
							FVector Scale(1.0, 1.0, 1.0);
							GetVector(Ref, TEXT("scale"), Scale);
							Instance.DrawScale3D = FVector3f(Scale);
							Built.Add(Instance);
						}
					}
					else
					{
						// Scatter: drop `count` points onto whatever is under a box.
						double Count = 0.0;
						if (!Body->TryGetNumberField(TEXT("count"), Count) || Count <= 0.0)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'instances' (explicit transforms) or 'count' (scatter that many "
									 "over the area) is required"));
							return;
						}
						if (Count > 10000.0)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("count_too_large"),
								TEXT("scatter at most 10000 instances per call"));
							return;
						}
						FVector Centre = FVector::ZeroVector;
						GetVector(Body, TEXT("center"), Centre);
						FVector Extent(1000.0, 1000.0, 0.0);
						GetVector(Body, TEXT("extent"), Extent);
						double TraceHeight = 100000.0;
						Body->TryGetNumberField(TEXT("trace_height"), TraceHeight);
						bool bAlignToNormal = false;
						Body->TryGetBoolField(TEXT("align_to_normal"), bAlignToNormal);
						double MinScale = 1.0;
						Body->TryGetNumberField(TEXT("min_scale"), MinScale);
						double MaxScale = MinScale;
						Body->TryGetNumberField(TEXT("max_scale"), MaxScale);
						bool bRandomYaw = true;
						Body->TryGetBoolField(TEXT("random_yaw"), bRandomYaw);
						double Seed = 0.0;
						FRandomStream Random(Body->TryGetNumberField(TEXT("seed"), Seed)
							? static_cast<int32>(Seed) : FMath::Rand());

						int32 Missed = 0;
						for (int32 Index = 0; Index < static_cast<int32>(Count); ++Index)
						{
							const FVector From(
								Centre.X + Random.FRandRange(-Extent.X, Extent.X),
								Centre.Y + Random.FRandRange(-Extent.Y, Extent.Y),
								Centre.Z + TraceHeight * 0.5);
							FHitResult Hit;
							if (!TraceDown(*World, From, TraceHeight, Hit))
							{
								++Missed;
								continue;
							}
							FFoliageInstance Instance;
							Instance.Location = Hit.ImpactPoint;
							Instance.Rotation = bAlignToNormal
								? Hit.ImpactNormal.Rotation() + FRotator(-90.0, 0.0, 0.0)
								: FRotator::ZeroRotator;
							if (bRandomYaw)
							{
								Instance.Rotation.Yaw += Random.FRandRange(0.0, 360.0);
							}
							const double Scale = Random.FRandRange(MinScale, MaxScale);
							Instance.DrawScale3D = FVector3f(static_cast<float>(Scale));
							Built.Add(Instance);
						}
						if (Built.Num() == 0)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("nothing_to_land_on"),
								FString::Printf(
									TEXT("all %d traces missed — scatter needs collision under the area ")
									TEXT("(a landscape or static meshes); check center/extent"),
									static_cast<int32>(Count)));
							return;
						}
						(void)Missed;
					}

					Actor->Modify();
					TArray<const FFoliageInstance*> Pointers;
					Pointers.Reserve(Built.Num());
					for (const FFoliageInstance& Instance : Built) { Pointers.Add(&Instance); }
					Info->AddInstances(Type, Pointers);
					Info->Refresh(/*Async=*/false, /*Force=*/true);

					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("type"), TypeName(*Type));
					Result->SetNumberField(TEXT("added"), Built.Num());
					Result->SetNumberField(TEXT("instance_count"), Info->Instances.Num());
					Responder->Ok(Result);
					return;
				}

				if (Operation == TEXT("remove_instances"))
				{
					TArray<int32> ToRemove;
					FVector Centre = FVector::ZeroVector;
					double Radius = 0.0;
					if (GetVector(Body, TEXT("center"), Centre) && Body->TryGetNumberField(TEXT("radius"), Radius))
					{
						const double RadiusSquared = Radius * Radius;
						for (int32 Index = 0; Index < Info->Instances.Num(); ++Index)
						{
							if (FVector::DistSquared(Info->Instances[Index].Location, Centre) <= RadiusSquared)
							{
								ToRemove.Add(Index);
							}
						}
					}
					else
					{
						ToRemove.Reserve(Info->Instances.Num());
						for (int32 Index = 0; Index < Info->Instances.Num(); ++Index)
						{
							ToRemove.Add(Index);
						}
					}

					Actor->Modify();
					Info->RemoveInstances(ToRemove, /*RebuildFoliageTree=*/true);

					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("type"), TypeName(*Type));
					Result->SetNumberField(TEXT("removed"), ToRemove.Num());
					Result->SetNumberField(TEXT("instance_count"), Info->Instances.Num());
					Responder->Ok(Result);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_types, add_type, remove_type, ")
						TEXT("add_instances, list_instances or remove_instances"),
						*Operation));
			});
	}
}
