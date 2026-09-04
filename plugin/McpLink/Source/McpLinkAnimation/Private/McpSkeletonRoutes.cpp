// Skeletons and Skeletal Meshes: bones, sockets, virtual bones, anim slots,
// LODs, material slots and morph targets.
//
// Sockets and virtual bones live on the skeleton and are shared by every mesh
// bound to it; LODs, material slots and morph targets belong to one mesh.
// USkeletalMeshEditorSubsystem is the engine's own scripting surface for the
// mesh half (it drives the real reduction module for LODs), so this route uses
// it rather than touching the render data directly.

#include "Animation/Skeleton.h"
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
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "ScopedTransaction.h"
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
						TEXT("assign_physics_asset, or save"),
						*Operation));
			});
	}
}
