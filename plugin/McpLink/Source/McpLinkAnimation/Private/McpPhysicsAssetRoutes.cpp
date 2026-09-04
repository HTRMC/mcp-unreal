// Physics Assets: ragdoll bodies and the constraints between them.
//
// A physics asset is a list of USkeletalBodySetups (one per simulated bone,
// each holding box/sphere/capsule/convex primitives) plus a list of
// UPhysicsConstraintTemplates. FPhysicsAssetUtils is the editor's own builder
// for both — including the automatic body generation the Physics Asset Tool
// runs when a mesh is first opened — so it does the work here too.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace PhysicsAssets
	{
		UPhysicsAsset* PhysicsAssetOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("asset"), Path, Responder,
					TEXT("a Physics Asset path, e.g. /Game/Characters/PHYS_Hero")))
			{
				return nullptr;
			}
			// A skeletal mesh stands in for the physics asset assigned to it.
			UObject* Found = ResolveAsset(Path);
			if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Found))
			{
				Found = Mesh->GetPhysicsAsset();
			}
			UPhysicsAsset* Asset = Cast<UPhysicsAsset>(Found);
			if (Asset == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
					FString::Printf(TEXT("no Physics Asset (or mesh with one) at '%s'"), *Path));
			}
			return Asset;
		}

		TSharedRef<FJsonObject> BodyToJson(const USkeletalBodySetup* Setup, int32 Index)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("index"), Index);
			Object->SetStringField(TEXT("bone"), Setup->BoneName.ToString());
			// Body settings — collision response, mass, damping, the primitives
			// themselves — are ordinary properties on this path.
			Object->SetStringField(TEXT("path"), Setup->GetPathName());
			const FKAggregateGeom& Geometry = Setup->AggGeom;
			Object->SetNumberField(TEXT("spheres"), Geometry.SphereElems.Num());
			Object->SetNumberField(TEXT("boxes"), Geometry.BoxElems.Num());
			Object->SetNumberField(TEXT("capsules"), Geometry.SphylElems.Num());
			Object->SetNumberField(TEXT("convex"), Geometry.ConvexElems.Num());
			Object->SetNumberField(TEXT("tapered_capsules"), Geometry.TaperedCapsuleElems.Num());
			Object->SetStringField(TEXT("physics_type"),
				StaticEnum<EPhysicsType>() != nullptr
					? StaticEnum<EPhysicsType>()->GetNameStringByValue(
						  static_cast<int64>(Setup->PhysicsType))
					: FString());
			return Object;
		}

		TSharedRef<FJsonObject> ConstraintToJson(const UPhysicsConstraintTemplate* Constraint, int32 Index)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("index"), Index);
			Object->SetStringField(TEXT("name"), Constraint->DefaultInstance.JointName.ToString());
			Object->SetStringField(TEXT("bone1"), Constraint->DefaultInstance.ConstraintBone1.ToString());
			Object->SetStringField(TEXT("bone2"), Constraint->DefaultInstance.ConstraintBone2.ToString());
			// Limits, drives and motion types live under DefaultInstance.
			Object->SetStringField(TEXT("path"), Constraint->GetPathName());
			return Object;
		}
	}

	using namespace PhysicsAssets;

	void RegisterPhysicsAssetRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/anim/physics"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path, MeshPath;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Characters/PHYS_Hero"))
						|| !RequireString(Body, TEXT("mesh"), MeshPath, Responder,
							TEXT("the Skeletal Mesh to build bodies from")))
					{
						return;
					}
					USkeletalMesh* Mesh = Cast<USkeletalMesh>(ResolveAsset(MeshPath));
					if (Mesh == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("mesh_not_found"),
							FString::Printf(TEXT("no Skeletal Mesh at '%s'"), *MeshPath));
						return;
					}
					if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}

					FPhysAssetCreateParams Params;
					Params.MinBoneSize = DoubleOr(Body, TEXT("min_bone_size"), Params.MinBoneSize);
					Params.bDisableCollisionsByDefault =
						BoolOr(Body, TEXT("disable_collisions"), Params.bDisableCollisionsByDefault);

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreatePhysicsAsset", "McpLink Create Physics Asset"));
					UPackage* Package = CreatePackage(*Path);
					UPhysicsAsset* Asset = NewObject<UPhysicsAsset>(
						Package, FName(*FPackageName::GetShortName(Path)),
						RF_Public | RF_Standalone | RF_Transactional);

					FText Error;
					// bShowProgress must be false: a progress dialog would block
					// a headless editor forever.
					const bool bCreated = FPhysicsAssetUtils::CreateFromSkeletalMesh(Asset, Mesh, Params,
						Error, BoolOr(Body, TEXT("assign"), true), /*bShowProgress*/ false);
					if (!bCreated)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"),
							Error.IsEmpty()
								? TEXT("could not generate bodies for that mesh")
								: *Error.ToString());
						return;
					}
					FAssetRegistryModule::AssetCreated(Asset);
					Package->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), Asset->GetPathName());
					Data->SetNumberField(TEXT("bodies"), Asset->SkeletalBodySetups.Num());
					Data->SetNumberField(TEXT("constraints"), Asset->ConstraintSetup.Num());
					Data->SetStringField(TEXT("message"),
						TEXT("created in memory — author it, then save"));
					Responder->Ok(Data);
					return;
				}

				UPhysicsAsset* Asset = PhysicsAssetOrError(Body, Responder);
				if (Asset == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), Asset->GetPathName());
					TArray<TSharedPtr<FJsonValue>> Bodies;
					for (int32 Index = 0; Index < Asset->SkeletalBodySetups.Num(); ++Index)
					{
						if (Asset->SkeletalBodySetups[Index] != nullptr)
						{
							Bodies.Add(MakeShared<FJsonValueObject>(
								BodyToJson(Asset->SkeletalBodySetups[Index], Index)));
						}
					}
					Data->SetArrayField(TEXT("bodies"), Bodies);

					TArray<TSharedPtr<FJsonValue>> Constraints;
					for (int32 Index = 0; Index < Asset->ConstraintSetup.Num(); ++Index)
					{
						if (Asset->ConstraintSetup[Index] != nullptr)
						{
							Constraints.Add(MakeShared<FJsonValueObject>(
								ConstraintToJson(Asset->ConstraintSetup[Index], Index)));
						}
					}
					Data->SetArrayField(TEXT("constraints"), Constraints);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Asset, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), Asset->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditPhysicsAsset", "McpLink Edit Physics Asset"));
				Asset->Modify();

				if (Operation == TEXT("add_body"))
				{
					FString Bone;
					if (!RequireString(Body, TEXT("bone"), Bone, Responder,
							TEXT("the bone to create a body on")))
					{
						return;
					}
					if (Asset->FindBodyIndex(FName(*Bone)) != INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("body_exists"),
							FString::Printf(TEXT("the asset already has a body on bone '%s'"), *Bone));
						return;
					}
					FPhysAssetCreateParams Params;
					const int32 Index =
						FPhysicsAssetUtils::CreateNewBody(Asset, FName(*Bone), Params);
					if (!Asset->SkeletalBodySetups.IsValidIndex(Index))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_failed"),
							FString::Printf(TEXT("could not create a body on bone '%s'"), *Bone));
						return;
					}
					Responder->Ok(BodyToJson(Asset->SkeletalBodySetups[Index], Index));
					return;
				}

				if (Operation == TEXT("remove_body"))
				{
					FString Bone;
					const int32 ByIndex = IntOr(Body, TEXT("index"), -1);
					int32 Index = ByIndex;
					if (Body->TryGetStringField(TEXT("bone"), Bone) && !Bone.IsEmpty())
					{
						Index = Asset->FindBodyIndex(FName(*Bone));
					}
					if (!Asset->SkeletalBodySetups.IsValidIndex(Index))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("body_not_found"),
							TEXT("pass 'bone' or a valid 'index' — see info"));
						return;
					}
					const FString Removed = Asset->SkeletalBodySetups[Index]->BoneName.ToString();
					// DestroyBody also drops the constraints that referenced it.
					FPhysicsAssetUtils::DestroyBody(Asset, Index);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Removed);
					Data->SetNumberField(TEXT("bodies"), Asset->SkeletalBodySetups.Num());
					Data->SetNumberField(TEXT("constraints"), Asset->ConstraintSetup.Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_constraint"))
				{
					FString Bone1, Bone2, Name;
					if (!RequireString(Body, TEXT("bone1"), Bone1, Responder,
							TEXT("the child bone — the constrained body"))
						|| !RequireString(Body, TEXT("bone2"), Bone2, Responder,
							TEXT("the parent bone")))
					{
						return;
					}
					Body->TryGetStringField(TEXT("name"), Name);
					const FName JointName = Name.IsEmpty() ? FName(*Bone1) : FName(*Name);
					const int32 Index = FPhysicsAssetUtils::CreateNewConstraint(Asset, JointName);
					if (!Asset->ConstraintSetup.IsValidIndex(Index))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_failed"),
							TEXT("could not create the constraint"));
						return;
					}
					UPhysicsConstraintTemplate* Constraint = Asset->ConstraintSetup[Index];
					Constraint->Modify();
					Constraint->DefaultInstance.ConstraintBone1 = FName(*Bone1);
					Constraint->DefaultInstance.ConstraintBone2 = FName(*Bone2);
					Responder->Ok(ConstraintToJson(Constraint, Index));
					return;
				}

				if (Operation == TEXT("remove_constraint"))
				{
					const int32 Index = IntOr(Body, TEXT("index"), -1);
					if (!Asset->ConstraintSetup.IsValidIndex(Index))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_index"),
							FString::Printf(TEXT("'index' must be 0..%d"), Asset->ConstraintSetup.Num() - 1));
						return;
					}
					const FString Removed =
						Asset->ConstraintSetup[Index]->DefaultInstance.JointName.ToString();
					FPhysicsAssetUtils::DestroyConstraint(Asset, Index);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Removed);
					Data->SetNumberField(TEXT("constraints"), Asset->ConstraintSetup.Num());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, info, add_body, remove_body, ")
						TEXT("add_constraint, remove_constraint, or save"),
						*Operation));
			});
	}
}
