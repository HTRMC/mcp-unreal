// Skeletons and Skeletal Meshes: bones, sockets, virtual bones, anim slots,
// LODs, material slots and morph targets.
//
// Sockets and virtual bones live on the skeleton and are shared by every mesh
// bound to it; LODs, material slots and morph targets belong to one mesh.
// USkeletalMeshEditorSubsystem is the engine's own scripting surface for the
// mesh half (it drives the real reduction module for LODs), so this route uses
// it rather than touching the render data directly.

#include "Animation/MorphTarget.h"
#include "BoneWeights.h"
#include "Animation/Skeleton.h"
#include "ClothingAsset.h"
#include "ClothingAssetBase.h"
#include "ClothingAssetFactoryInterface.h"
#include "ClothingSystemEditorModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Materials/MaterialInterface.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "MeshDescription.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "ScopedTransaction.h"
#include "SkeletalMeshAttributes.h"
#include "SkinWeightsAttributesRef.h"
#include "SkeletalMeshEditorSubsystem.h"
#include "AnimationBlueprintLibrary.h"

namespace McpLink
{
	namespace SkeletonRoutes
	{
		USkeleton* SkeletonOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("skeleton"), Path, Responder,
					TEXT("a Skeleton asset path, e.g. /Game/Characters/SK_Hero_Skeleton")))
			{
				return nullptr;
			}
			// A skeletal mesh is accepted as a stand-in for its skeleton, since
			// that is usually the asset an agent has in hand.
			UObject* Asset = ResolveAsset(Path);
			if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
			{
				Asset = Mesh->GetSkeleton();
			}
			USkeleton* Skeleton = Cast<USkeleton>(Asset);
			if (Skeleton == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("skeleton_not_found"),
					FString::Printf(TEXT("no Skeleton (or Skeletal Mesh with one) at '%s'"), *Path));
			}
			return Skeleton;
		}

		USkeletalMesh* MeshOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("mesh"), Path, Responder,
					TEXT("a Skeletal Mesh asset path, e.g. /Game/Characters/SK_Hero")))
			{
				return nullptr;
			}
			USkeletalMesh* Mesh = Cast<USkeletalMesh>(ResolveAsset(Path));
			if (Mesh == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("mesh_not_found"),
					FString::Printf(TEXT("no Skeletal Mesh at '%s'"), *Path));
			}
			return Mesh;
		}

		TSharedRef<FJsonObject> SocketToJson(const USkeletalMeshSocket* Socket)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Socket->SocketName.ToString());
			Object->SetStringField(TEXT("bone"), Socket->BoneName.ToString());
			Object->SetArrayField(TEXT("location"), VectorToJson(Socket->RelativeLocation));
			Object->SetArrayField(TEXT("rotation"), RotatorToJson(Socket->RelativeRotation));
			Object->SetArrayField(TEXT("scale"), VectorToJson(Socket->RelativeScale));
			// Socket settings are ordinary properties on this path.
			Object->SetStringField(TEXT("path"), Socket->GetPathName());
			return Object;
		}

		USkeletalMeshSocket* AddSocket(
			UObject* Owner,
			TArray<TObjectPtr<USkeletalMeshSocket>>& Sockets,
			const TSharedRef<FJsonObject>& Body,
			FName Name,
			FName Bone)
		{
			USkeletalMeshSocket* Socket =
				NewObject<USkeletalMeshSocket>(Owner, NAME_None, RF_Transactional);
			Socket->SocketName = Name;
			Socket->BoneName = Bone;
			GetVector(Body, TEXT("location"), Socket->RelativeLocation);
			GetRotator(Body, TEXT("rotation"), Socket->RelativeRotation);
			if (!GetVector(Body, TEXT("scale"), Socket->RelativeScale))
			{
				Socket->RelativeScale = FVector::OneVector;
			}
			Sockets.Add(Socket);
			return Socket;
		}
	}

	using namespace SkeletonRoutes;

	namespace MeshEdits
	{
		bool ReadVector3f(const TSharedPtr<FJsonValue>& Value, FVector3f& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Value.IsValid() || !Value->TryGetArray(Items) || Items->Num() != 3)
			{
				return false;
			}
			Out = FVector3f(static_cast<float>((*Items)[0]->AsNumber()),
				static_cast<float>((*Items)[1]->AsNumber()), static_cast<float>((*Items)[2]->AsNumber()));
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Vector3fJson(const FVector3f& V)
		{
			return {MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)};
		}

		/// A clothing asset on the mesh by name or guid, or nullptr after responding.
		UClothingAssetCommon* ClothingOrError(
			USkeletalMesh& Mesh, const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			if (!RequireString(Body, TEXT("clothing"), Spec, Responder, TEXT("a clothing asset name or guid from list_clothing")))
			{
				return nullptr;
			}
			for (UClothingAssetBase* Asset : Mesh.GetMeshClothingAssets())
			{
				if (Asset != nullptr && (Asset->GetName() == Spec || Asset->GetAssetGuid().ToString() == Spec))
				{
					return Cast<UClothingAssetCommon>(Asset);
				}
			}
			Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("clothing_not_found"),
				FString::Printf(TEXT("'%s' has no clothing asset '%s' — list_clothing shows them"), *Mesh.GetPathName(), *Spec));
			return nullptr;
		}

		TSharedRef<FJsonObject> ClothingJson(USkeletalMesh& Mesh, UClothingAssetBase& Asset)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Asset.GetName());
			Entry->SetStringField(TEXT("guid"), Asset.GetAssetGuid().ToString());
			if (const UClothingAssetCommon* Common = Cast<UClothingAssetCommon>(&Asset))
			{
				Entry->SetNumberField(TEXT("lods"), Common->GetNumLods());
				Entry->SetStringField(TEXT("physics_asset"),
					Common->PhysicsAsset != nullptr ? Common->PhysicsAsset->GetPathName() : FString());
			}
			TArray<TSharedPtr<FJsonValue>> Bound;
			if (const FSkeletalMeshModel* Model = Mesh.GetImportedModel())
			{
				for (int32 LodIndex = 0; LodIndex < Model->LODModels.Num(); ++LodIndex)
				{
					const FSkeletalMeshLODModel& Lod = Model->LODModels[LodIndex];
					for (int32 SectionIndex = 0; SectionIndex < Lod.Sections.Num(); ++SectionIndex)
					{
						if (Mesh.GetSectionClothingAsset(LodIndex, SectionIndex) == &Asset)
						{
							const TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
							Section->SetNumberField(TEXT("lod"), LodIndex);
							Section->SetNumberField(TEXT("section"), SectionIndex);
							Section->SetNumberField(TEXT("asset_lod"), Lod.Sections[SectionIndex].ClothingData.AssetLodIndex);
							Bound.Add(MakeShared<FJsonValueObject>(Section));
						}
					}
				}
			}
			Entry->SetArrayField(TEXT("bound_sections"), Bound);
			return Entry;
		}

		TArray<TSharedPtr<FJsonValue>> ClothingListJson(USkeletalMesh& Mesh)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (UClothingAssetBase* Asset : Mesh.GetMeshClothingAssets())
			{
				if (Asset != nullptr)
				{
					Out.Add(MakeShared<FJsonValueObject>(ClothingJson(Mesh, *Asset)));
				}
			}
			return Out;
		}

		/// The LOD's imported model, or nullptr after responding.
		const FSkeletalMeshLODModel* LodModelOrError(
			USkeletalMesh& Mesh, int32 Lod, const TSharedRef<FMcpResponder>& Responder)
		{
			const FSkeletalMeshModel* Model = Mesh.GetImportedModel();
			if (Model == nullptr || !Model->LODModels.IsValidIndex(Lod))
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("lod_not_found"),
					FString::Printf(TEXT("'%s' has no LOD %d"), *Mesh.GetPathName(), Lod));
				return nullptr;
			}
			return &Model->LODModels[Lod];
		}
	}

	void RegisterSkeletonRoutes(FMcpLinkCoreModule& Core)
	{
		// ------------------------------------------------------- skeleton ---
		Core.RegisterRoute(TEXT("/api/anim/skeleton"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				USkeleton* Skeleton = SkeletonOrError(Body, Responder);
				if (Skeleton == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());

					const FReferenceSkeleton& Reference = Skeleton->GetReferenceSkeleton();
					const int32 MaxBones = FMath::Clamp(IntOr(Body, TEXT("max_bones"), 200), 1, 2000);
					TArray<TSharedPtr<FJsonValue>> Bones;
					for (int32 Index = 0; Index < Reference.GetNum() && Bones.Num() < MaxBones; ++Index)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Reference.GetBoneName(Index).ToString());
						const int32 Parent = Reference.GetParentIndex(Index);
						Item->SetStringField(TEXT("parent"),
							Parent == INDEX_NONE ? FString() : Reference.GetBoneName(Parent).ToString());
						Bones.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetNumberField(TEXT("bone_count"), Reference.GetNum());
					Data->SetArrayField(TEXT("bones"), Bones);

					TArray<TSharedPtr<FJsonValue>> Sockets;
					for (const TObjectPtr<USkeletalMeshSocket>& Socket : Skeleton->Sockets)
					{
						if (Socket != nullptr)
						{
							Sockets.Add(MakeShared<FJsonValueObject>(SocketToJson(Socket)));
						}
					}
					Data->SetArrayField(TEXT("sockets"), Sockets);

					TArray<TSharedPtr<FJsonValue>> VirtualBones;
					for (const FVirtualBone& Bone : Skeleton->GetVirtualBones())
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Bone.VirtualBoneName.ToString());
						Item->SetStringField(TEXT("source"), Bone.SourceBoneName.ToString());
						Item->SetStringField(TEXT("target"), Bone.TargetBoneName.ToString());
						VirtualBones.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("virtual_bones"), VirtualBones);

					TArray<TSharedPtr<FJsonValue>> Groups;
					for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("group"), Group.GroupName.ToString());
						TArray<TSharedPtr<FJsonValue>> Slots;
						for (const FName& Slot : Group.SlotNames)
						{
							Slots.Add(MakeShared<FJsonValueString>(Slot.ToString()));
						}
						Item->SetArrayField(TEXT("slots"), Slots);
						Groups.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("slot_groups"), Groups);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditSkeleton", "McpLink Edit Skeleton"));
				Skeleton->Modify();

				if (Operation == TEXT("add_socket") || Operation == TEXT("remove_socket"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the socket name")))
					{
						return;
					}
					if (Operation == TEXT("remove_socket"))
					{
						const int32 Removed = Skeleton->Sockets.RemoveAll(
							[&Name](const TObjectPtr<USkeletalMeshSocket>& Socket)
							{ return Socket != nullptr && Socket->SocketName == FName(*Name); });
						if (Removed == 0)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("socket_not_found"),
								FString::Printf(TEXT("skeleton has no socket '%s'"), *Name));
							return;
						}
						Skeleton->MarkPackageDirty();
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("removed"), Name);
						Responder->Ok(Data);
						return;
					}

					FString Bone;
					if (!RequireString(Body, TEXT("bone"), Bone, Responder,
							TEXT("the bone to attach the socket to")))
					{
						return;
					}
					if (Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(*Bone)) == INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("bone_not_found"),
							FString::Printf(TEXT("skeleton has no bone '%s' — see info"), *Bone));
						return;
					}
					if (Skeleton->FindSocket(FName(*Name)) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("socket_exists"),
							FString::Printf(TEXT("skeleton already has a socket '%s'"), *Name));
						return;
					}
					USkeletalMeshSocket* Socket =
						AddSocket(Skeleton, Skeleton->Sockets, Body, FName(*Name), FName(*Bone));
					Skeleton->MarkPackageDirty();
					Responder->Ok(SocketToJson(Socket));
					return;
				}

				if (Operation == TEXT("add_virtual_bone") || Operation == TEXT("remove_virtual_bone"))
				{
					if (Operation == TEXT("remove_virtual_bone"))
					{
						FString Name;
						if (!RequireString(Body, TEXT("name"), Name, Responder,
								TEXT("the virtual bone name")))
						{
							return;
						}
						if (!VirtualBoneNameHelpers::CheckVirtualBonePrefix(Name))
						{
							Name = VirtualBoneNameHelpers::AddVirtualBonePrefix(Name);
						}
						Skeleton->RemoveVirtualBones({FName(*Name)});
						Skeleton->MarkPackageDirty();
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("removed"), Name);
						Responder->Ok(Data);
						return;
					}
					FString Source, Target, Name;
					if (!RequireString(Body, TEXT("source"), Source, Responder, TEXT("the source bone"))
						|| !RequireString(Body, TEXT("target"), Target, Responder, TEXT("the target bone")))
					{
						return;
					}
					const bool bNamed = Body->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty();
					// AddNewNamedVirtualBone rejects any name without the engine's
					// virtual-bone prefix, so apply it rather than failing on it.
					if (bNamed && !VirtualBoneNameHelpers::CheckVirtualBonePrefix(Name))
					{
						Name = VirtualBoneNameHelpers::AddVirtualBonePrefix(Name);
					}
					FName Created(*Name);
					const bool bAdded = bNamed
						? Skeleton->AddNewNamedVirtualBone(FName(*Source), FName(*Target), Created)
						: Skeleton->AddNewVirtualBone(FName(*Source), FName(*Target), Created);
					if (!bAdded)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
							FString::Printf(
								TEXT("could not add a virtual bone from '%s' to '%s' — check both bones ")
								TEXT("exist, and that no virtual bone already joins that pair or uses the ")
								TEXT("name '%s'"),
								*Source, *Target, *Name));
						return;
					}
					Skeleton->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("name"), Created.ToString());
					Data->SetStringField(TEXT("source"), Source);
					Data->SetStringField(TEXT("target"), Target);
					Data->SetNumberField(TEXT("virtual_bone_count"), Skeleton->GetVirtualBones().Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_slot") || Operation == TEXT("remove_slot")
					|| Operation == TEXT("add_slot_group") || Operation == TEXT("set_slot_group"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder,
							TEXT("the slot or slot-group name")))
					{
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					if (Operation == TEXT("add_slot"))
					{
						Skeleton->RegisterSlotNode(FName(*Name));
						Data->SetStringField(TEXT("slot"), Name);
						Data->SetStringField(TEXT("group"),
							Skeleton->GetSlotGroupName(FName(*Name)).ToString());
					}
					else if (Operation == TEXT("remove_slot"))
					{
						Skeleton->RemoveSlotName(FName(*Name));
						Data->SetStringField(TEXT("removed"), Name);
					}
					else if (Operation == TEXT("add_slot_group"))
					{
						if (!Skeleton->AddSlotGroupName(FName(*Name)))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("group_exists"),
								FString::Printf(TEXT("skeleton already has a slot group '%s'"), *Name));
							return;
						}
						Data->SetStringField(TEXT("group"), Name);
					}
					else
					{
						FString Group;
						if (!RequireString(Body, TEXT("group"), Group, Responder,
								TEXT("the slot group to move the slot into")))
						{
							return;
						}
						Skeleton->SetSlotGroupName(FName(*Name), FName(*Group));
						Data->SetStringField(TEXT("slot"), Name);
						Data->SetStringField(TEXT("group"), Group);
					}
					Skeleton->MarkPackageDirty();
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_preview_mesh"))
				{
					FString MeshPath;
					if (!RequireString(Body, TEXT("mesh"), MeshPath, Responder,
							TEXT("a Skeletal Mesh path to preview this skeleton with")))
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
					UAnimationBlueprintLibrary::SetSkeletonPreviewMesh(Skeleton, Mesh);
					Skeleton->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
					Data->SetStringField(TEXT("preview_mesh"), Mesh->GetPathName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Skeleton, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use info, add_socket, remove_socket, ")
						TEXT("add_virtual_bone, remove_virtual_bone, add_slot, remove_slot, ")
						TEXT("add_slot_group, set_slot_group, set_preview_mesh, or save"),
						*Operation));
			});

		// -------------------------------------------------- skeletal mesh ---
		Core.RegisterRoute(TEXT("/api/anim/mesh"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				USkeletalMesh* Mesh = MeshOrError(Body, Responder);
				if (Mesh == nullptr)
				{
					return;
				}
				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetStringField(TEXT("skeleton"),
						Mesh->GetSkeleton() != nullptr ? Mesh->GetSkeleton()->GetPathName() : FString());
					Data->SetStringField(TEXT("physics_asset"),
						Mesh->GetPhysicsAsset() != nullptr ? Mesh->GetPhysicsAsset()->GetPathName() : FString());
					Data->SetNumberField(TEXT("lod_count"), Mesh->GetLODNum());

					TArray<TSharedPtr<FJsonValue>> Materials;
					const TArray<FSkeletalMaterial>& MaterialSlots = Mesh->GetMaterials();
					for (int32 Index = 0; Index < MaterialSlots.Num(); ++Index)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetNumberField(TEXT("index"), Index);
						Item->SetStringField(TEXT("slot"), MaterialSlots[Index].MaterialSlotName.ToString());
						Item->SetStringField(TEXT("material"),
							MaterialSlots[Index].MaterialInterface != nullptr
								? MaterialSlots[Index].MaterialInterface->GetPathName()
								: FString());
						Materials.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("material_slots"), Materials);

					TArray<TSharedPtr<FJsonValue>> Sockets;
					for (const TObjectPtr<USkeletalMeshSocket>& Socket : Mesh->GetMeshOnlySocketList())
					{
						if (Socket != nullptr)
						{
							Sockets.Add(MakeShared<FJsonValueObject>(SocketToJson(Socket)));
						}
					}
					Data->SetArrayField(TEXT("mesh_sockets"), Sockets);

					TArray<TSharedPtr<FJsonValue>> Morphs;
					const int32 MaxMorphs = FMath::Clamp(IntOr(Body, TEXT("max_morph_targets"), 100), 1, 2000);
					for (const TObjectPtr<UMorphTarget>& Morph : Mesh->GetMorphTargets())
					{
						if (Morph == nullptr || Morphs.Num() >= MaxMorphs)
						{
							continue;
						}
						Morphs.Add(MakeShared<FJsonValueString>(Morph->GetName()));
					}
					Data->SetNumberField(TEXT("morph_target_count"), Mesh->GetMorphTargets().Num());
					Data->SetArrayField(TEXT("morph_targets"), Morphs);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditSkeletalMesh", "McpLink Edit Skeletal Mesh"));
				Mesh->Modify();

				if (Operation == TEXT("regenerate_lod"))
				{
					const int32 Count = IntOr(Body, TEXT("lod_count"), Mesh->GetLODNum());
					if (!USkeletalMeshEditorSubsystem::RegenerateLOD(Mesh, Count, /*bRegenerateEvenIfImported*/ false,
							/*bGenerateBaseLOD*/ false))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("lod_failed"),
							TEXT("the reduction module refused to regenerate the LOD chain"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetNumberField(TEXT("lod_count"), Mesh->GetLODNum());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_lods"))
				{
					const TArray<TSharedPtr<FJsonValue>>* Indices = nullptr;
					TArray<int32> ToRemove;
					if (Body->TryGetArrayField(TEXT("lods"), Indices) && Indices != nullptr)
					{
						for (const TSharedPtr<FJsonValue>& Value : *Indices)
						{
							double Number = 0.0;
							if (Value.IsValid() && Value->TryGetNumber(Number))
							{
								ToRemove.Add(static_cast<int32>(Number));
							}
						}
					}
					if (ToRemove.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'lods' must be an array of LOD indices, e.g. [2, 3]"));
						return;
					}
					if (!USkeletalMeshEditorSubsystem::RemoveLODs(Mesh, ToRemove))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("remove_failed"),
							TEXT("those LODs could not be removed (LOD 0 cannot be)"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("lod_count"), Mesh->GetLODNum());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_material"))
				{
					const int32 Index = IntOr(Body, TEXT("index"), -1);
					TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
					if (!Materials.IsValidIndex(Index))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_index"),
							FString::Printf(TEXT("'index' must be 0..%d"), Materials.Num() - 1));
						return;
					}
					FString MaterialPath, SlotName;
					if (Body->TryGetStringField(TEXT("material"), MaterialPath) && !MaterialPath.IsEmpty())
					{
						UMaterialInterface* Material = Cast<UMaterialInterface>(ResolveAsset(MaterialPath));
						if (Material == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("material_not_found"),
								FString::Printf(TEXT("no material at '%s'"), *MaterialPath));
							return;
						}
						Materials[Index].MaterialInterface = Material;
					}
					if (Body->TryGetStringField(TEXT("slot"), SlotName) && !SlotName.IsEmpty())
					{
						Materials[Index].MaterialSlotName = FName(*SlotName);
					}
					Mesh->PostEditChange();
					Mesh->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("index"), Index);
					Data->SetStringField(TEXT("slot"), Materials[Index].MaterialSlotName.ToString());
					Data->SetStringField(TEXT("material"),
						Materials[Index].MaterialInterface != nullptr
							? Materials[Index].MaterialInterface->GetPathName()
							: FString());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_socket") || Operation == TEXT("remove_socket")
					|| Operation == TEXT("rename_socket"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the socket name")))
					{
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					if (Operation == TEXT("remove_socket"))
					{
						if (!USkeletalMeshEditorSubsystem::RemoveSocket(Mesh, FName(*Name)))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("socket_not_found"),
								FString::Printf(TEXT("mesh has no socket '%s'"), *Name));
							return;
						}
						Data->SetStringField(TEXT("removed"), Name);
					}
					else if (Operation == TEXT("rename_socket"))
					{
						FString NewName;
						if (!RequireString(Body, TEXT("new_name"), NewName, Responder,
								TEXT("the new socket name")))
						{
							return;
						}
						if (!USkeletalMeshEditorSubsystem::RenameSocket(Mesh, FName(*Name), FName(*NewName)))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("rename_failed"),
								FString::Printf(TEXT("could not rename socket '%s' to '%s'"),
									*Name, *NewName));
							return;
						}
						Data->SetStringField(TEXT("socket"), NewName);
					}
					else
					{
						FString Bone;
						if (!RequireString(Body, TEXT("bone"), Bone, Responder,
								TEXT("the bone to attach the socket to")))
						{
							return;
						}
						if (Mesh->FindSocket(FName(*Name)) != nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("socket_exists"),
								FString::Printf(TEXT("mesh already has a socket '%s'"), *Name));
							return;
						}
						USkeletalMeshSocket* Socket = AddSocket(
							Mesh, Mesh->GetMeshOnlySocketList(), Body, FName(*Name), FName(*Bone));
						return Responder->Ok(SocketToJson(Socket));
					}
					Mesh->MarkPackageDirty();
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("assign_physics_asset"))
				{
					FString AssetPath;
					if (!RequireString(Body, TEXT("physics_asset"), AssetPath, Responder,
							TEXT("a Physics Asset path — physics_asset_ops create makes one")))
					{
						return;
					}
					UPhysicsAsset* Asset = Cast<UPhysicsAsset>(ResolveAsset(AssetPath));
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
							FString::Printf(TEXT("no Physics Asset at '%s'"), *AssetPath));
						return;
					}
					if (!USkeletalMeshEditorSubsystem::AssignPhysicsAsset(Mesh, Asset))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("assign_failed"),
							TEXT("that physics asset is not compatible with this mesh"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("physics_asset"), Asset->GetPathName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("get_skin_weights") || Operation == TEXT("set_skin_weights"))
				{
					using namespace MeshEdits;
					const int32 Lod = IntOr(Body, TEXT("lod"), 0);
					if (LodModelOrError(*Mesh, Lod, Responder) == nullptr)
					{
						return;
					}
					FMeshDescription* MeshDescription = Mesh->GetMeshDescription(Lod);
					if (MeshDescription == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_mesh_description"),
							FString::Printf(TEXT("LOD %d of '%s' has no source mesh description"), Lod, *Mesh->GetPathName()));
						return;
					}
					FSkeletalMeshAttributes Attributes(*MeshDescription);
					FString ProfileName;
					Body->TryGetStringField(TEXT("profile"), ProfileName);
					const FName Profile = ProfileName.IsEmpty() ? NAME_None : FName(*ProfileName);
					const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
					if (Operation == TEXT("get_skin_weights"))
					{
						const FSkeletalMeshConstAttributes ConstAttributes(*MeshDescription);
						FSkinWeightsVertexAttributesConstRef Weights = ConstAttributes.GetVertexSkinWeights(Profile);
						if (!Weights.IsValid())
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("profile_not_found"),
								FString::Printf(TEXT("LOD %d has no skin weight profile '%s'"), Lod, *ProfileName));
							return;
						}
						TArray<int32> VertexIds;
						const TArray<TSharedPtr<FJsonValue>>* Requested = nullptr;
						if (Body->TryGetArrayField(TEXT("vertices"), Requested))
						{
							for (const TSharedPtr<FJsonValue>& Value : *Requested)
							{
								VertexIds.Add(static_cast<int32>(Value->AsNumber()));
							}
						}
						else
						{
							const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_vertices"), 64), 1, 100000);
							for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
							{
								if (VertexIds.Num() >= Max)
								{
									break;
								}
								VertexIds.Add(VertexID.GetValue());
							}
						}
						TArray<TSharedPtr<FJsonValue>> Out;
						for (const int32 Id : VertexIds)
						{
							const FVertexID VertexID(Id);
							if (!MeshDescription->IsVertexValid(VertexID))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_vertex"),
									FString::Printf(TEXT("vertex %d is not a vertex of LOD %d — it has %d"), Id, Lod, MeshDescription->Vertices().Num()));
								return;
							}
							const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
							Entry->SetNumberField(TEXT("vertex"), Id);
							TArray<TSharedPtr<FJsonValue>> Bones;
							for (const UE::AnimationCore::FBoneWeight BoneWeight : Weights.Get(VertexID))
							{
								const TSharedRef<FJsonObject> BoneJson = MakeShared<FJsonObject>();
								BoneJson->SetStringField(TEXT("bone"), RefSkeleton.IsValidIndex(BoneWeight.GetBoneIndex())
									? RefSkeleton.GetBoneName(BoneWeight.GetBoneIndex()).ToString() : FString());
								BoneJson->SetNumberField(TEXT("index"), BoneWeight.GetBoneIndex());
								BoneJson->SetNumberField(TEXT("weight"), BoneWeight.GetWeight());
								Bones.Add(MakeShared<FJsonValueObject>(BoneJson));
							}
							Entry->SetArrayField(TEXT("bones"), Bones);
							Out.Add(MakeShared<FJsonValueObject>(Entry));
						}
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
						Data->SetNumberField(TEXT("lod"), Lod);
						Data->SetNumberField(TEXT("vertex_count"), MeshDescription->Vertices().Num());
						TArray<TSharedPtr<FJsonValue>> Profiles;
						for (const FName& Name : Attributes.GetSkinWeightProfileNames(true))
						{
							Profiles.Add(MakeShared<FJsonValueString>(Name.ToString()));
						}
						Data->SetArrayField(TEXT("profiles"), Profiles);
						Data->SetArrayField(TEXT("weights"), Out);
						Responder->Ok(Data);
						return;
					}

					// set_skin_weights: [{vertex, bones: [{bone, weight}]}] — the
					// weights are normalized and sorted by the engine's own
					// FBoneWeights, then the build turns them into render data.
					const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
					if (!Body->TryGetArrayField(TEXT("weights"), Entries) || Entries->IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'weights' is required: [{vertex, bones: [{bone, weight}, ...]}, ...]"));
						return;
					}
					struct FVertexWeights
					{
						FVertexID Vertex;
						TArray<UE::AnimationCore::FBoneWeight> Bones;
					};
					TArray<FVertexWeights> Parsed;
					for (const TSharedPtr<FJsonValue>& Value : *Entries)
					{
						const TSharedPtr<FJsonObject>* Entry = nullptr;
						int32 Vertex = -1;
						const TArray<TSharedPtr<FJsonValue>>* BoneValues = nullptr;
						if (!Value->TryGetObject(Entry) || !(*Entry)->TryGetNumberField(TEXT("vertex"), Vertex)
							|| !(*Entry)->TryGetArrayField(TEXT("bones"), BoneValues) || BoneValues->IsEmpty())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_weights"),
								TEXT("each entry is {vertex, bones: [{bone, weight}, ...]}"));
							return;
						}
						if (Vertex < 0 || !MeshDescription->IsVertexValid(FVertexID(Vertex)))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_vertex"),
								FString::Printf(TEXT("vertex %d is not a vertex of LOD %d — it has %d"), Vertex, Lod, MeshDescription->Vertices().Num()));
							return;
						}
						FVertexWeights VertexWeights;
						VertexWeights.Vertex = FVertexID(Vertex);
						for (const TSharedPtr<FJsonValue>& BoneValue : *BoneValues)
						{
							const TSharedPtr<FJsonObject>* BoneEntry = nullptr;
							FString BoneName;
							double Weight = 0.0;
							if (!BoneValue->TryGetObject(BoneEntry) || !(*BoneEntry)->TryGetStringField(TEXT("bone"), BoneName)
								|| !(*BoneEntry)->TryGetNumberField(TEXT("weight"), Weight))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_weights"),
									TEXT("each bone entry is {bone (name), weight (0-1)}"));
								return;
							}
							const int32 BoneIndex = RefSkeleton.FindBoneIndex(FName(*BoneName));
							if (BoneIndex == INDEX_NONE)
							{
								Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("bone_not_found"),
									FString::Printf(TEXT("the mesh has no bone '%s'"), *BoneName));
								return;
							}
							VertexWeights.Bones.Emplace(static_cast<FBoneIndexType>(BoneIndex), static_cast<float>(Weight));
						}
						Parsed.Add(MoveTemp(VertexWeights));
					}
					{
						FScopedSkeletalMeshPostEditChange PostEditChangeScope(Mesh);
						Mesh->ModifyMeshDescription(Lod);
						if (!Profile.IsNone() && !Attributes.GetVertexSkinWeights(Profile).IsValid())
						{
							Attributes.RegisterSkinWeightAttribute(Profile);
						}
						FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights(Profile);
						if (!Weights.IsValid())
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("profile_not_found"),
								FString::Printf(TEXT("LOD %d has no skin weight profile '%s'"), Lod, *ProfileName));
							return;
						}
						for (const FVertexWeights& VertexWeights : Parsed)
						{
							Weights.Set(VertexWeights.Vertex, UE::AnimationCore::FBoneWeights::Create(VertexWeights.Bones));
						}
						Mesh->CommitMeshDescription(Lod);
					}
					Mesh->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetNumberField(TEXT("lod"), Lod);
					Data->SetStringField(TEXT("profile"), Profile.IsNone() ? TEXT("default") : ProfileName);
					Data->SetNumberField(TEXT("vertices_written"), Parsed.Num());
					Data->SetStringField(TEXT("note"), TEXT("weights were normalized to sum to one; the render data was rebuilt"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("morph_target_info"))
				{
					using namespace MeshEdits;
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("a morph target name from info")))
					{
						return;
					}
					UMorphTarget* Morph = Mesh->FindMorphTarget(FName(*Name));
					if (Morph == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("morph_target_not_found"),
							FString::Printf(TEXT("'%s' has no morph target '%s'"), *Mesh->GetPathName(), *Name));
						return;
					}
					const int32 Lod = IntOr(Body, TEXT("lod"), 0);
					const int32 MaxDeltas = FMath::Clamp(IntOr(Body, TEXT("max_deltas"), 64), 0, 100000);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetStringField(TEXT("name"), Morph->GetName());
					TArray<TSharedPtr<FJsonValue>> Lods;
					const int32 LodCount = Mesh->GetImportedModel() != nullptr ? Mesh->GetImportedModel()->LODModels.Num() : 0;
					for (int32 Index = 0; Index < LodCount; ++Index)
					{
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetNumberField(TEXT("lod"), Index);
						Entry->SetBoolField(TEXT("has_data"), Morph->HasDataForLOD(Index));
						Entry->SetNumberField(TEXT("deltas"), Morph->HasDataForLOD(Index) ? Morph->GetNumDeltasForLOD(Index) : 0);
						Entry->SetBoolField(TEXT("generated_by_engine"), Morph->IsGeneratedByEngine(Index));
						Lods.Add(MakeShared<FJsonValueObject>(Entry));
					}
					Data->SetArrayField(TEXT("lods"), Lods);
					// The source of truth is the LOD's mesh description: vertex
					// ids there are what add_morph_target takes, and what the
					// build turns into render deltas.
					TArray<TSharedPtr<FJsonValue>> Deltas;
					int32 SourceDeltas = 0;
					if (FMeshDescription* MeshDescription = Mesh->GetMeshDescription(Lod))
					{
						FSkeletalMeshAttributes Attributes(*MeshDescription);
						const FName MorphName = Morph->GetFName();
						if (Attributes.HasMorphTargetPositionsAttribute(MorphName))
						{
							TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexMorphPositionDelta(MorphName);
							for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
							{
								const FVector3f Delta = Positions[VertexID];
								if (Delta.IsNearlyZero())
								{
									continue;
								}
								++SourceDeltas;
								if (Deltas.Num() >= MaxDeltas)
								{
									continue;
								}
								const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
								Entry->SetNumberField(TEXT("vertex"), VertexID.GetValue());
								Entry->SetArrayField(TEXT("delta"), Vector3fJson(Delta));
								Deltas.Add(MakeShared<FJsonValueObject>(Entry));
							}
						}
						Data->SetNumberField(TEXT("vertex_count"), MeshDescription->Vertices().Num());
						Data->SetBoolField(TEXT("has_normals"), Attributes.HasMorphTargetNormalsAttribute(MorphName));
					}
					Data->SetNumberField(TEXT("sample_lod"), Lod);
					Data->SetNumberField(TEXT("source_deltas"), SourceDeltas);
					Data->SetArrayField(TEXT("sample"), Deltas);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_morph_target"))
				{
					using namespace MeshEdits;
					FString NameString;
					if (!RequireString(Body, TEXT("name"), NameString, Responder, TEXT("the morph target's name")))
					{
						return;
					}
					const FName Name(*NameString);
					const int32 Lod = IntOr(Body, TEXT("lod"), 0);
					if (LodModelOrError(*Mesh, Lod, Responder) == nullptr)
					{
						return;
					}
					// Morph targets are built from the LOD's mesh description in
					// 5.8: a UMorphTarget registered by hand is discarded by the
					// next build, so the deltas go into the source and the build
					// produces the render-side morph target.
					FMeshDescription* MeshDescription = Mesh->GetMeshDescription(Lod);
					if (MeshDescription == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_mesh_description"),
							FString::Printf(TEXT("LOD %d of '%s' has no source mesh description to add a morph target to"),
								Lod, *Mesh->GetPathName()));
						return;
					}
					const TArray<TSharedPtr<FJsonValue>>* DeltaValues = nullptr;
					if (!Body->TryGetArrayField(TEXT("deltas"), DeltaValues) || DeltaValues->IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'deltas' is required: [{vertex, delta: [x, y, z], normal?: [x, y, z]}, ...] ")
							TEXT("over the LOD's source vertices (morph_target_info reports vertex_count)"));
						return;
					}
					struct FSourceDelta
					{
						FVertexID Vertex;
						FVector3f Position;
						FVector3f Normal;
						bool bHasNormal = false;
					};
					TArray<FSourceDelta> Deltas;
					Deltas.Reserve(DeltaValues->Num());
					bool bAnyNormal = false;
					for (const TSharedPtr<FJsonValue>& Value : *DeltaValues)
					{
						const TSharedPtr<FJsonObject>* Entry = nullptr;
						int32 Vertex = -1;
						FSourceDelta Delta;
						if (!Value->TryGetObject(Entry) || !(*Entry)->TryGetNumberField(TEXT("vertex"), Vertex)
							|| !ReadVector3f((*Entry)->TryGetField(TEXT("delta")), Delta.Position))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_delta"),
								TEXT("each delta is {vertex, delta: [x, y, z], normal?: [x, y, z]}"));
							return;
						}
						if (Vertex < 0 || !MeshDescription->IsVertexValid(FVertexID(Vertex)))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_vertex"),
								FString::Printf(TEXT("vertex %d is not a vertex of LOD %d — it has %d source vertices"),
									Vertex, Lod, MeshDescription->Vertices().Num()));
							return;
						}
						Delta.Vertex = FVertexID(Vertex);
						Delta.bHasNormal = ReadVector3f((*Entry)->TryGetField(TEXT("normal")), Delta.Normal);
						bAnyNormal |= Delta.bHasNormal;
						Deltas.Add(Delta);
					}
					FSkeletalMeshAttributes Attributes(*MeshDescription);
					const bool bExists = Attributes.HasMorphTargetPositionsAttribute(Name);
					const bool bReplace = BoolOr(Body, TEXT("replace"), false);
					if (bExists && !bReplace)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("'%s' already has a morph target '%s' — pass replace=true to overwrite its LOD %d deltas"),
								*Mesh->GetPathName(), *NameString, Lod));
						return;
					}
					{
						// Rebuilds the render data when it goes out of scope.
						FScopedSkeletalMeshPostEditChange PostEditChangeScope(Mesh);
						Mesh->ModifyMeshDescription(Lod);
						if (bExists && bAnyNormal && !Attributes.HasMorphTargetNormalsAttribute(Name))
						{
							Attributes.UnregisterMorphTargetAttribute(Name);
						}
						if (!Attributes.HasMorphTargetPositionsAttribute(Name))
						{
							Attributes.RegisterMorphTargetAttribute(Name, bAnyNormal);
						}
						TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexMorphPositionDelta(Name);
						if (bReplace)
						{
							for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
							{
								Positions[VertexID] = FVector3f::ZeroVector;
							}
						}
						for (const FSourceDelta& Delta : Deltas)
						{
							Positions[Delta.Vertex] = Delta.Position;
						}
						if (Attributes.HasMorphTargetNormalsAttribute(Name))
						{
							TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceMorphNormalDelta(Name);
							for (const FSourceDelta& Delta : Deltas)
							{
								if (Delta.bHasNormal)
								{
									for (const FVertexInstanceID InstanceID : MeshDescription->GetVertexVertexInstanceIDs(Delta.Vertex))
									{
										Normals[InstanceID] = Delta.Normal;
									}
								}
							}
						}
						Mesh->CommitMeshDescription(Lod);
					}
					Mesh->MarkPackageDirty();
					const UMorphTarget* Built = Mesh->FindMorphTarget(Name);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetStringField(TEXT("name"), NameString);
					Data->SetNumberField(TEXT("lod"), Lod);
					Data->SetNumberField(TEXT("vertex_count"), MeshDescription->Vertices().Num());
					Data->SetNumberField(TEXT("deltas_given"), Deltas.Num());
					Data->SetBoolField(TEXT("built"), Built != nullptr);
					Data->SetNumberField(TEXT("render_deltas"),
						Built != nullptr && Built->HasDataForLOD(Lod) ? Built->GetNumDeltasForLOD(Lod) : 0);
					Data->SetBoolField(TEXT("replaced"), bExists);
					Data->SetNumberField(TEXT("morph_target_count"), Mesh->GetMorphTargets().Num());
					Data->SetStringField(TEXT("note"),
						TEXT("drive it from an animation curve of the same name, or SetMorphTarget on a skeletal mesh component"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_morph_target"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("a morph target name from info")))
					{
						return;
					}
					UMorphTarget* Morph = Mesh->FindMorphTarget(FName(*Name));
					if (Morph == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("morph_target_not_found"),
							FString::Printf(TEXT("'%s' has no morph target '%s'"), *Mesh->GetPathName(), *Name));
						return;
					}
					if (!Mesh->RemoveMorphTargets({FName(*Name)}, /*bInRebuildRenderMesh*/ true))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("remove_refused"),
							FString::Printf(TEXT("the mesh refused to remove morph target '%s'"), *Name));
						return;
					}
					Mesh->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetStringField(TEXT("removed"), Name);
					Data->SetNumberField(TEXT("morph_target_count"), Mesh->GetMorphTargets().Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_clothing"))
				{
					using namespace MeshEdits;
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetArrayField(TEXT("clothing"), ClothingListJson(*Mesh));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_clothing"))
				{
					using namespace MeshEdits;
					const int32 Lod = IntOr(Body, TEXT("lod"), 0);
					const int32 Section = IntOr(Body, TEXT("section"), -1);
					const FSkeletalMeshLODModel* LodModel = LodModelOrError(*Mesh, Lod, Responder);
					if (LodModel == nullptr)
					{
						return;
					}
					if (!LodModel->Sections.IsValidIndex(Section))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_section"),
							FString::Printf(TEXT("'section' is required — LOD %d has %d sections"), Lod, LodModel->Sections.Num()));
						return;
					}
					if (Mesh->GetSectionClothingAsset(Lod, Section) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("section_has_clothing"),
							FString::Printf(TEXT("LOD %d section %d already has clothing bound — unbind_clothing first"), Lod, Section));
						return;
					}
					FString Name;
					Body->TryGetStringField(TEXT("name"), Name);
					if (Name.IsEmpty())
					{
						Name = FString::Printf(TEXT("%s_Cloth_LOD%d_%d"), *Mesh->GetName(), Lod, Section);
					}
					UClothingAssetFactoryBase* Factory =
						FModuleManager::LoadModuleChecked<FClothingSystemEditorModule>("ClothingSystemEditor").GetFactory();
					if (Factory == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_cloth_factory"),
							TEXT("the Clothing System editor module has no asset factory"));
						return;
					}
					FSkeletalMeshClothBuildParams Params;
					Params.TargetAsset = nullptr;
					Params.TargetLod = 0;
					Params.bRemapParameters = false;
					Params.AssetName = Name;
					Params.LodIndex = Lod;
					Params.SourceSection = Section;
					Params.bRemoveFromMesh = BoolOr(Body, TEXT("remove_section"), false);
					// The Skeletal Mesh Editor's "Create Clothing Data from
					// Section": a simulation mesh from the section's triangles,
					// weighted to the bones the section uses.
					UClothingAssetBase* Asset = Factory->CreateFromSkeletalMesh(Mesh, Params);
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("create_failed"),
							TEXT("the clothing factory refused the section — see the log"));
						return;
					}
					if (!Mesh->GetMeshClothingAssets().Contains(Asset))
					{
						Mesh->AddClothingAsset(Asset);
					}
					Mesh->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = ClothingJson(*Mesh, *Asset);
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetStringField(TEXT("message"),
						TEXT("created — bind_clothing applies it to a section; its physics live on the asset's ")
						TEXT("ClothConfigs (set_property) and a Physics Asset for collision"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("bind_clothing") || Operation == TEXT("unbind_clothing"))
				{
					using namespace MeshEdits;
					const int32 Lod = IntOr(Body, TEXT("lod"), 0);
					const int32 Section = IntOr(Body, TEXT("section"), -1);
					const FSkeletalMeshLODModel* LodModel = LodModelOrError(*Mesh, Lod, Responder);
					if (LodModel == nullptr)
					{
						return;
					}
					if (!LodModel->Sections.IsValidIndex(Section))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_section"),
							FString::Printf(TEXT("'section' is required — LOD %d has %d sections"), Lod, LodModel->Sections.Num()));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetNumberField(TEXT("lod"), Lod);
					Data->SetNumberField(TEXT("section"), Section);
					if (Operation == TEXT("bind_clothing"))
					{
						UClothingAssetCommon* Cloth = ClothingOrError(*Mesh, Body, Responder);
						if (Cloth == nullptr)
						{
							return;
						}
						const int32 AssetLod = IntOr(Body, TEXT("asset_lod"), 0);
						if (!Cloth->IsValidLod(AssetLod))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_asset_lod"),
								FString::Printf(TEXT("clothing '%s' has %d LODs"), *Cloth->GetName(), Cloth->GetNumLods()));
							return;
						}
						// The build regenerates the LOD model from the source, so
						// the binding also has to live in the section user data
						// it reads — what Persona's clothing combo box writes.
						FSkeletalMeshLODModel& LodModelMutable = Mesh->GetImportedModel()->LODModels[Lod];
						FSkelMeshSourceSectionUserData& UserData =
							LodModelMutable.UserSectionsData.FindOrAdd(LodModelMutable.Sections[Section].OriginalDataSectionIndex);
						if (UClothingAssetBase* Current = Mesh->GetSectionClothingAsset(Lod, Section))
						{
							Current->Modify();
							Current->UnbindFromSkeletalMesh(Mesh, Lod, Section);
							UserData.CorrespondClothAssetIndex = INDEX_NONE;
							UserData.ClothingData.AssetGuid = FGuid();
							UserData.ClothingData.AssetLodIndex = INDEX_NONE;
						}
						Cloth->Modify();
						if (!Cloth->BindToSkeletalMesh(Mesh, Lod, Section, AssetLod))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("bind_refused"),
								TEXT("the clothing asset refused the section — its simulation mesh must match the ")
								TEXT("section's vertices (create it from the same section), and the section must be free"));
							return;
						}
						int32 AssetIndex = INDEX_NONE;
						Mesh->GetMeshClothingAssets().Find(Cloth, AssetIndex);
						UserData.CorrespondClothAssetIndex = static_cast<int16>(AssetIndex);
						UserData.ClothingData.AssetGuid = Cloth->GetAssetGuid();
						UserData.ClothingData.AssetLodIndex = AssetLod;
						Data->SetStringField(TEXT("clothing"), Cloth->GetName());
						Data->SetNumberField(TEXT("asset_lod"), AssetLod);
					}
					else
					{
						UClothingAssetBase* Current = Mesh->GetSectionClothingAsset(Lod, Section);
						if (Current == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_bound"),
								FString::Printf(TEXT("LOD %d section %d has no clothing bound"), Lod, Section));
							return;
						}
						Current->Modify();
						Current->UnbindFromSkeletalMesh(Mesh, Lod, Section);
						FSkeletalMeshLODModel& LodModelMutable = Mesh->GetImportedModel()->LODModels[Lod];
						FSkelMeshSourceSectionUserData& UserData =
							LodModelMutable.UserSectionsData.FindOrAdd(LodModelMutable.Sections[Section].OriginalDataSectionIndex);
						UserData.CorrespondClothAssetIndex = INDEX_NONE;
						UserData.ClothingData.AssetGuid = FGuid();
						UserData.ClothingData.AssetLodIndex = INDEX_NONE;
					}
					Mesh->PostEditChange();
					Mesh->MarkPackageDirty();
					Data->SetArrayField(TEXT("clothing_assets"), ClothingListJson(*Mesh));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_clothing"))
				{
					using namespace MeshEdits;
					UClothingAssetCommon* Cloth = ClothingOrError(*Mesh, Body, Responder);
					if (Cloth == nullptr)
					{
						return;
					}
					Cloth->Modify();
					if (FSkeletalMeshModel* Model = Mesh->GetImportedModel())
					{
						for (int32 Lod = 0; Lod < Model->LODModels.Num(); ++Lod)
						{
							FSkeletalMeshLODModel& LodModelMutable = Model->LODModels[Lod];
							for (int32 Section = 0; Section < LodModelMutable.Sections.Num(); ++Section)
							{
								if (Mesh->GetSectionClothingAsset(Lod, Section) != Cloth)
								{
									continue;
								}
								Cloth->UnbindFromSkeletalMesh(Mesh, Lod, Section);
								FSkelMeshSourceSectionUserData& UserData =
									LodModelMutable.UserSectionsData.FindOrAdd(LodModelMutable.Sections[Section].OriginalDataSectionIndex);
								UserData.CorrespondClothAssetIndex = INDEX_NONE;
								UserData.ClothingData.AssetGuid = FGuid();
								UserData.ClothingData.AssetLodIndex = INDEX_NONE;
							}
						}
					}
					Mesh->GetMeshClothingAssets().Remove(Cloth);
					Cloth->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
					Cloth->MarkAsGarbage();
					Mesh->PostEditChange();
					Mesh->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetArrayField(TEXT("clothing_assets"), ClothingListJson(*Mesh));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Mesh, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use info, regenerate_lod, remove_lods, ")
						TEXT("set_material, add_socket, remove_socket, rename_socket, ")
						TEXT("assign_physics_asset, morph_target_info, add_morph_target, remove_morph_target, ")
						TEXT("list_clothing, create_clothing, bind_clothing, unbind_clothing, remove_clothing, ")
						TEXT("get_skin_weights, set_skin_weights, or save"),
						*Operation));
			});
	}
}
