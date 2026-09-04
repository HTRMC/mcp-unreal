// Static Mesh editing: LODs, collision, Nanite, lightmap UVs, sockets and
// material slots.
//
// Everything routes through UStaticMeshEditorSubsystem, which is what the
// Static Mesh editor's own buttons call — so LOD reduction runs the real
// reduction module and collision generation runs the real convex decomposition,
// rather than approximating either.

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "JsonObjectConverter.h"
#include "Materials/MaterialInterface.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScopedTransaction.h"
#include "StaticMeshEditorSubsystem.h"
#include "StaticMeshEditorSubsystemHelpers.h"

namespace McpLink
{
	namespace
	{
		UStaticMeshEditorSubsystem* MeshSubsystem()
		{
			return GEditor ? GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>() : nullptr;
		}

		UStaticMesh* MeshOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("mesh"), Path, Responder,
				TEXT("a Static Mesh asset path, e.g. /Engine/BasicShapes/Cube")))
			{
				return nullptr;
			}
			UObject* Object = ResolveObject(Path);
			if (Object == nullptr && !Path.Contains(TEXT(".")))
			{
				Object = ResolveObject(
					FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path)));
			}
			UStaticMesh* Mesh = Cast<UStaticMesh>(Object);
			if (Mesh == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("mesh_not_found"),
					FString::Printf(TEXT("no Static Mesh at '%s'"), *Path));
			}
			return Mesh;
		}

		bool ParseShape(const FString& Spec, EScriptCollisionShapeType& OutShape)
		{
			static const TMap<FString, EScriptCollisionShapeType> Shapes = {
				{TEXT("box"), EScriptCollisionShapeType::Box},
				{TEXT("sphere"), EScriptCollisionShapeType::Sphere},
				{TEXT("capsule"), EScriptCollisionShapeType::Capsule},
				{TEXT("ndop10x"), EScriptCollisionShapeType::NDOP10_X},
				{TEXT("ndop10y"), EScriptCollisionShapeType::NDOP10_Y},
				{TEXT("ndop10z"), EScriptCollisionShapeType::NDOP10_Z},
				{TEXT("ndop18"), EScriptCollisionShapeType::NDOP18},
				{TEXT("ndop26"), EScriptCollisionShapeType::NDOP26},
			};
			const FString Key = Spec.ToLower().Replace(TEXT("_"), TEXT(""));
			if (const EScriptCollisionShapeType* Found = Shapes.Find(Key))
			{
				OutShape = *Found;
				return true;
			}
			return false;
		}

		TSharedRef<FJsonObject> SocketToJson(const UStaticMeshSocket* Socket)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Socket->SocketName.ToString());
			Object->SetArrayField(TEXT("location"), VectorToJson(Socket->RelativeLocation));
			Object->SetArrayField(TEXT("rotation"), RotatorToJson(Socket->RelativeRotation));
			Object->SetArrayField(TEXT("scale"), VectorToJson(Socket->RelativeScale));
			Object->SetStringField(TEXT("tag"), Socket->Tag);
			Object->SetStringField(TEXT("path"), Socket->GetPathName());
			return Object;
		}
	}

	void RegisterStaticMeshRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/mesh/static"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				UStaticMeshEditorSubsystem* Subsystem = MeshSubsystem();
				if (Subsystem == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_subsystem"),
						TEXT("the Static Mesh editor subsystem is unavailable"));
					return;
				}
				UStaticMesh* Mesh = MeshOrError(Body, Responder);
				if (!Mesh) { return; }

				if (Operation == TEXT("get_info"))
				{
					const int32 LodCount = Subsystem->GetLodCount(Mesh);
					TArray<TSharedPtr<FJsonValue>> Lods;
					const TArray<float> ScreenSizes = Subsystem->GetLodScreenSizes(Mesh);
					for (int32 Index = 0; Index < LodCount; ++Index)
					{
						const TSharedRef<FJsonObject> Lod = MakeShared<FJsonObject>();
						Lod->SetNumberField(TEXT("index"), Index);
						Lod->SetNumberField(TEXT("vertices"), Subsystem->GetNumberVerts(Mesh, Index));
						Lod->SetNumberField(TEXT("uv_channels"), Subsystem->GetNumUVChannels(Mesh, Index));
						if (ScreenSizes.IsValidIndex(Index))
						{
							Lod->SetNumberField(TEXT("screen_size"), ScreenSizes[Index]);
						}
						Lods.Add(MakeShared<FJsonValueObject>(Lod));
					}

					TArray<TSharedPtr<FJsonValue>> Materials;
					for (int32 Index = 0; Index < Mesh->GetStaticMaterials().Num(); ++Index)
					{
						const FStaticMaterial& Slot = Mesh->GetStaticMaterials()[Index];
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetNumberField(TEXT("slot"), Index);
						Item->SetStringField(TEXT("name"), Slot.MaterialSlotName.ToString());
						Item->SetStringField(TEXT("material"),
							Slot.MaterialInterface ? Slot.MaterialInterface->GetPathName() : TEXT(""));
						Materials.Add(MakeShared<FJsonValueObject>(Item));
					}

					TArray<TSharedPtr<FJsonValue>> Sockets;
					for (const UStaticMeshSocket* Socket : Mesh->Sockets)
					{
						if (Socket != nullptr)
						{
							Sockets.Add(MakeShared<FJsonValueObject>(SocketToJson(Socket)));
						}
					}

					const FMeshNaniteSettings Nanite = Subsystem->GetNaniteSettings(Mesh);
					const TSharedRef<FJsonObject> NaniteJson = MakeShared<FJsonObject>();
					NaniteJson->SetBoolField(TEXT("enabled"), Nanite.bEnabled);
					NaniteJson->SetNumberField(TEXT("position_precision"), Nanite.PositionPrecision);
					NaniteJson->SetNumberField(TEXT("keep_percent_triangles"), Nanite.KeepPercentTriangles);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Mesh->GetPathName());
					Data->SetStringField(TEXT("lod_group"), Subsystem->GetLODGroup(Mesh).ToString());
					Data->SetArrayField(TEXT("lods"), Lods);
					Data->SetArrayField(TEXT("materials"), Materials);
					Data->SetArrayField(TEXT("sockets"), Sockets);
					Data->SetObjectField(TEXT("nanite"), NaniteJson);
					Data->SetNumberField(
						TEXT("simple_collision_count"), Subsystem->GetSimpleCollisionCount(Mesh));
					Data->SetNumberField(
						TEXT("convex_collision_count"), Subsystem->GetConvexCollisionCount(Mesh));
					Data->SetBoolField(TEXT("has_vertex_colors"), Subsystem->HasVertexColors(Mesh));
					// Everything else on a mesh is a plain property.
					Data->SetStringField(TEXT("note"),
						TEXT("other settings (build settings, distance field, ray tracing) are properties ")
						TEXT("on this path — get_property / set_property reach them"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_lods"))
				{
					const TArray<TSharedPtr<FJsonValue>>* LodValues = nullptr;
					if (!Body->TryGetArrayField(TEXT("lods"), LodValues) || LodValues == nullptr
						|| LodValues->IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'lods' is required — one entry per LOD, each with percent_triangles ")
							TEXT("(0-1) and optionally screen_size; the first entry is LOD 0"));
						return;
					}
					FStaticMeshReductionOptions Options;
					Options.bAutoComputeLODScreenSize = BoolOr(Body, TEXT("auto_screen_size"), true);
					for (const TSharedPtr<FJsonValue>& Value : *LodValues)
					{
						const TSharedPtr<FJsonObject>* Entry = nullptr;
						if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_lods"),
								TEXT("each 'lods' entry must be an object"));
							return;
						}
						FStaticMeshReductionSettings Settings;
						Settings.PercentTriangles = static_cast<float>(
							FMath::Clamp(DoubleOr(Entry->ToSharedRef(), TEXT("percent_triangles"), 1.0), 0.0, 1.0));
						Settings.ScreenSize = static_cast<float>(
							FMath::Clamp(DoubleOr(Entry->ToSharedRef(), TEXT("screen_size"), 0.5), 0.0, 1.0));
						Options.ReductionSettings.Add(Settings);
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetLods", "McpLink Set Static Mesh LODs"));
					const int32 Result = Subsystem->SetLodsWithNotification(
						Mesh, Options, /*bApplyChanges*/ true);
					if (Result < 0)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("set_lods_failed"),
							TEXT("LOD generation failed — check get_log for the reduction module's errors"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("lod_count"), Subsystem->GetLodCount(Mesh));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_lods"))
				{
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RemoveLods", "McpLink Remove Static Mesh LODs"));
					const bool bRemoved = Subsystem->RemoveLods(Mesh);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("removed"), bRemoved);
					Data->SetNumberField(TEXT("lod_count"), Subsystem->GetLodCount(Mesh));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_lod_group"))
				{
					FString Group;
					if (!RequireString(Body, TEXT("lod_group"), Group, Responder,
						TEXT("a LOD group name from the project's [/Script/Engine.StaticMesh] settings, e.g. LargeProp")))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetLodGroup", "McpLink Set LOD Group"));
					if (!Subsystem->SetLODGroup(Mesh, FName(*Group)))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_lod_group"),
							FString::Printf(
								TEXT("'%s' is not a configured LOD group — config_ops can list ")
								TEXT("[/Script/Engine.StaticMesh] LODGroups from BaseEngine.ini"),
								*Group));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("lod_group"), Subsystem->GetLODGroup(Mesh).ToString());
					Data->SetNumberField(TEXT("lod_count"), Subsystem->GetLodCount(Mesh));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_collision"))
				{
					FString ShapeSpec;
					Body->TryGetStringField(TEXT("shape"), ShapeSpec);
					EScriptCollisionShapeType Shape = EScriptCollisionShapeType::Box;
					if (!ShapeSpec.IsEmpty() && !ParseShape(ShapeSpec, Shape))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_shape"),
							FString::Printf(
								TEXT("unknown shape '%s' — use box, sphere, capsule, ndop10x, ndop10y, ")
								TEXT("ndop10z, ndop18 or ndop26"),
								*ShapeSpec));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddCollision", "McpLink Add Collision"));
					const int32 Index =
						Subsystem->AddSimpleCollisionsWithNotification(Mesh, Shape, /*bApplyChanges*/ true);
					if (Index == INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_failed"),
							TEXT("could not add the collision shape"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("index"), Index);
					Data->SetNumberField(
						TEXT("simple_collision_count"), Subsystem->GetSimpleCollisionCount(Mesh));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_convex_collision"))
				{
					const int32 HullCount = FMath::Clamp(IntOr(Body, TEXT("hull_count"), 4), 1, 64);
					const int32 MaxHullVerts = FMath::Clamp(IntOr(Body, TEXT("max_hull_verts"), 16), 6, 32);
					const int32 Precision = FMath::Clamp(IntOr(Body, TEXT("hull_precision"), 100000), 10000, 1000000);
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddConvexCollision", "McpLink Add Convex Collision"));
					if (!Subsystem->SetConvexDecompositionCollisionsWithNotification(
						Mesh, HullCount, MaxHullVerts, Precision, /*bApplyChanges*/ true))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("decomposition_failed"),
							TEXT("convex decomposition failed — check get_log"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(
						TEXT("convex_collision_count"), Subsystem->GetConvexCollisionCount(Mesh));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_collision"))
				{
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RemoveCollision", "McpLink Remove Collision"));
					const bool bRemoved =
						Subsystem->RemoveCollisionsWithNotification(Mesh, /*bApplyChanges*/ true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("removed"), bRemoved);
					Data->SetNumberField(
						TEXT("simple_collision_count"), Subsystem->GetSimpleCollisionCount(Mesh));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_nanite"))
				{
					FMeshNaniteSettings Settings = Subsystem->GetNaniteSettings(Mesh);
					Settings.bEnabled = BoolOr(Body, TEXT("enabled"), Settings.bEnabled);
					Settings.PositionPrecision =
						IntOr(Body, TEXT("position_precision"), Settings.PositionPrecision);
					Settings.KeepPercentTriangles = static_cast<float>(FMath::Clamp(
						DoubleOr(Body, TEXT("keep_percent_triangles"), Settings.KeepPercentTriangles),
						0.0, 1.0));
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetNanite", "McpLink Set Nanite Settings"));
					Subsystem->SetNaniteSettings(Mesh, Settings, /*bApplyChanges*/ true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("enabled"), Subsystem->GetNaniteSettings(Mesh).bEnabled);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_lightmap_uvs"))
				{
					const bool bGenerate = BoolOr(Body, TEXT("generate"), true);
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetLightmapUvs", "McpLink Set Lightmap UVs"));
					const bool bChanged = Subsystem->SetGenerateLightmapUVs(Mesh, bGenerate);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("generate_lightmap_uvs"), bGenerate);
					Data->SetBoolField(TEXT("changed"), bChanged);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_socket") || Operation == TEXT("set_socket"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("socket"), Name, Responder))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddSocket", "McpLink Add Static Mesh Socket"));
					Mesh->Modify();
					UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*Name));
					const bool bCreated = Socket == nullptr;
					if (bCreated)
					{
						if (Operation == TEXT("set_socket"))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("socket_not_found"),
								FString::Printf(TEXT("no socket '%s' — use add_socket"), *Name));
							return;
						}
						Socket = NewObject<UStaticMeshSocket>(Mesh);
						Socket->SetFlags(RF_Transactional);
						Socket->SocketName = FName(*Name);
						Mesh->AddSocket(Socket);
					}
					Socket->Modify();
					FVector Location;
					if (GetVector(Body, TEXT("location"), Location))
					{
						Socket->RelativeLocation = Location;
					}
					FRotator Rotation;
					if (GetRotator(Body, TEXT("rotation"), Rotation))
					{
						Socket->RelativeRotation = Rotation;
					}
					FVector Scale;
					if (GetVector(Body, TEXT("scale"), Scale))
					{
						Socket->RelativeScale = Scale;
					}
					FString Tag;
					if (Body->TryGetStringField(TEXT("tag"), Tag))
					{
						Socket->Tag = Tag;
					}
					Mesh->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = SocketToJson(Socket);
					Data->SetBoolField(TEXT("created"), bCreated);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_socket"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("socket"), Name, Responder))
					{
						return;
					}
					UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*Name));
					if (Socket == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("socket_not_found"),
							FString::Printf(TEXT("no socket '%s' on %s"), *Name, *Mesh->GetName()));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RemoveSocket", "McpLink Remove Static Mesh Socket"));
					Mesh->Modify();
					Mesh->RemoveSocket(Socket);
					Mesh->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Name);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_material"))
				{
					const int32 Slot = IntOr(Body, TEXT("slot"), 0);
					FString MaterialPath;
					if (!RequireString(Body, TEXT("material"), MaterialPath, Responder,
						TEXT("a Material or Material Instance asset path")))
					{
						return;
					}
					UMaterialInterface* Material = Cast<UMaterialInterface>(ResolveObject(
						MaterialPath.Contains(TEXT(".")) ? MaterialPath
							: FString::Printf(TEXT("%s.%s"), *MaterialPath,
								*FPackageName::GetShortName(MaterialPath))));
					if (Material == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("material_not_found"),
							FString::Printf(TEXT("no material at '%s'"), *MaterialPath));
						return;
					}
					if (!Mesh->GetStaticMaterials().IsValidIndex(Slot))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_slot"),
							FString::Printf(
								TEXT("slot %d does not exist — this mesh has %d material slots"),
								Slot, Mesh->GetStaticMaterials().Num()));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetMeshMaterial", "McpLink Set Static Mesh Material"));
					Mesh->Modify();
					Mesh->SetMaterial(Slot, Material);
					Mesh->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("slot"), Slot);
					Data->SetStringField(TEXT("material"), Material->GetPathName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Mesh, Filename, Error))
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
						TEXT("unknown operation '%s' — use get_info, set_lods, remove_lods, ")
						TEXT("set_lod_group, add_collision, add_convex_collision, remove_collision, ")
						TEXT("set_nanite, set_lightmap_uvs, add_socket, set_socket, remove_socket, ")
						TEXT("set_material or save"),
						*Operation));
			});
	}
}
