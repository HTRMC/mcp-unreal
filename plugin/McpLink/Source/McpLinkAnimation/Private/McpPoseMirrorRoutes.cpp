// Pose Assets and Mirror Data Tables: two animation assets that are built
// from a skeleton and an animation rather than authored in a graph.

#include "Animation/AnimSequence.h"
#include "Animation/AnimationSettings.h"
#include "Animation/MirrorDataTable.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace PoseMirror
	{
		UPackage* PackageForNewAsset(
			const FString& Path, const TSharedRef<FMcpResponder>& Responder, FName& OutName)
		{
			if (!FPackageName::IsValidLongPackageName(Path))
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_path"),
					FString::Printf(TEXT("'%s' is not a package path — use /Game/Folder/Name"), *Path));
				return nullptr;
			}
			if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
					FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
				return nullptr;
			}
			OutName = FName(*FPackageName::GetShortName(Path));
			return CreatePackage(*Path);
		}

		USkeleton* SkeletonFromSpec(const FString& Spec)
		{
			UObject* Asset = ResolveAsset(Spec);
			if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
			{
				return Mesh->GetSkeleton();
			}
			if (UAnimationAsset* Anim = Cast<UAnimationAsset>(Asset))
			{
				return Anim->GetSkeleton();
			}
			return Cast<USkeleton>(Asset);
		}

		UPoseAsset* PoseAssetOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("pose_asset"), Path, Responder, TEXT("a Pose Asset path")))
			{
				return nullptr;
			}
			UPoseAsset* Asset = Cast<UPoseAsset>(ResolveAsset(Path));
			if (Asset == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pose_asset_not_found"),
					FString::Printf(TEXT("no Pose Asset at '%s'"), *Path));
			}
			return Asset;
		}

		UMirrorDataTable* MirrorTableOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("table"), Path, Responder, TEXT("a Mirror Data Table path")))
			{
				return nullptr;
			}
			UMirrorDataTable* Table = Cast<UMirrorDataTable>(ResolveAsset(Path));
			if (Table == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("table_not_found"),
					FString::Printf(TEXT("no Mirror Data Table at '%s'"), *Path));
			}
			return Table;
		}

		bool ReadNameList(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, TArray<FName>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Body->TryGetArrayField(Field, Values))
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Name;
				if (Value->TryGetString(Name) && !Name.IsEmpty())
				{
					Out.Add(FName(*Name));
				}
			}
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> NamesJson(const TArray<FName>& Names, int32 Max)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (int32 Index = 0; Index < Names.Num() && Index < Max; ++Index)
			{
				Out.Add(MakeShared<FJsonValueString>(Names[Index].ToString()));
			}
			return Out;
		}

		TSharedRef<FJsonObject> PoseAssetJson(const UPoseAsset& Asset, int32 MaxNames)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("pose_asset"), Asset.GetPathName());
			Data->SetStringField(TEXT("skeleton"),
				Asset.GetSkeleton() != nullptr ? Asset.GetSkeleton()->GetPathName() : FString());
			Data->SetNumberField(TEXT("num_poses"), Asset.GetNumPoses());
			Data->SetNumberField(TEXT("num_curves"), Asset.GetNumCurves());
			Data->SetNumberField(TEXT("num_tracks"), Asset.GetNumTracks());
			Data->SetArrayField(TEXT("poses"), NamesJson(Asset.GetPoseFNames(), MaxNames));
			Data->SetArrayField(TEXT("curves"), NamesJson(Asset.GetCurveFNames(), MaxNames));
			Data->SetArrayField(TEXT("tracks"), NamesJson(Asset.GetTrackNames(), MaxNames));
			Data->SetBoolField(TEXT("additive"), Asset.IsValidAdditive());
			const int32 BaseIndex = Asset.GetBasePoseIndex();
			if (Asset.IsValidAdditive() && Asset.GetPoseFNames().IsValidIndex(BaseIndex))
			{
				Data->SetStringField(TEXT("base_pose"), Asset.GetPoseFNames()[BaseIndex].ToString());
			}
			Data->SetNumberField(TEXT("base_pose_index"), BaseIndex);
			return Data;
		}

		TSharedRef<FJsonObject> MirrorTableJson(const UMirrorDataTable& Table, int32 MaxRows)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("table"), Table.GetPathName());
			Data->SetStringField(TEXT("skeleton"),
				Table.Skeleton != nullptr ? Table.Skeleton->GetPathName() : FString());
			Data->SetStringField(TEXT("axis"), StaticEnum<EAxis::Type>()->GetNameStringByValue(Table.MirrorAxis));
			switch (Table.GetSkeletonSyncStatus())
			{
			case UMirrorDataTable::ESyncStatus::UpToDate: Data->SetStringField(TEXT("sync"), TEXT("up_to_date")); break;
			case UMirrorDataTable::ESyncStatus::Stale: Data->SetStringField(TEXT("sync"), TEXT("stale")); break;
			default: Data->SetStringField(TEXT("sync"), TEXT("never_synced")); break;
			}
			TArray<TSharedPtr<FJsonValue>> Expressions;
			for (const FMirrorFindReplaceExpression& Expression : Table.MirrorFindReplaceExpressions)
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("find"), Expression.FindExpression.ToString());
				Entry->SetStringField(TEXT("replace"), Expression.ReplaceExpression.ToString());
				Entry->SetStringField(TEXT("method"),
					StaticEnum<EMirrorFindReplaceMethod::Type>()->GetNameStringByValue(Expression.FindReplaceMethod));
				Expressions.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("expressions"), Expressions);

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Total = 0;
			for (const TPair<FName, uint8*>& Pair : Table.GetRowMap())
			{
				++Total;
				if (Rows.Num() >= MaxRows || Pair.Value == nullptr)
				{
					continue;
				}
				const FMirrorTableRow* Row = reinterpret_cast<const FMirrorTableRow*>(Pair.Value);
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("row"), Pair.Key.ToString());
				Entry->SetStringField(TEXT("name"), Row->Name.ToString());
				Entry->SetStringField(TEXT("mirrored_name"), Row->MirroredName.ToString());
				Entry->SetStringField(TEXT("type"),
					StaticEnum<EMirrorRowType::Type>()->GetNameStringByValue(Row->MirrorEntryType));
				Entry->SetBoolField(TEXT("enabled"), Row->bEnabled);
				Rows.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetNumberField(TEXT("row_count"), Total);
			Data->SetArrayField(TEXT("rows"), Rows);
			return Data;
		}

		bool ReadExpressions(const TSharedRef<FJsonObject>& Body, TArray<FMirrorFindReplaceExpression>& Out, FString& Error)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Body->TryGetArrayField(TEXT("expressions"), Values))
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				FString Find, Replace, Method = TEXT("Prefix");
				if (!Value->TryGetObject(Entry) || !(*Entry)->TryGetStringField(TEXT("find"), Find)
					|| !(*Entry)->TryGetStringField(TEXT("replace"), Replace))
				{
					Error = TEXT("each expression is {find, replace, method?} with method Prefix, Suffix or RegularExpression");
					return false;
				}
				(*Entry)->TryGetStringField(TEXT("method"), Method);
				const int64 MethodValue = StaticEnum<EMirrorFindReplaceMethod::Type>()->GetValueByNameString(Method);
				if (MethodValue == INDEX_NONE)
				{
					Error = FString::Printf(TEXT("'%s' is not a find/replace method — Prefix, Suffix or RegularExpression"), *Method);
					return false;
				}
				Out.Emplace(FName(*Find), FName(*Replace), static_cast<EMirrorFindReplaceMethod::Type>(MethodValue));
			}
			return true;
		}

		bool ReadAxis(const TSharedRef<FJsonObject>& Body, EAxis::Type& Out, FString& Error)
		{
			FString Axis;
			if (!Body->TryGetStringField(TEXT("axis"), Axis))
			{
				return true;
			}
			const int64 Value = StaticEnum<EAxis::Type>()->GetValueByNameString(Axis);
			if (Value == INDEX_NONE || Value == EAxis::None)
			{
				Error = FString::Printf(TEXT("'%s' is not a mirror axis — X, Y or Z"), *Axis);
				return false;
			}
			Out = static_cast<EAxis::Type>(Value);
			return true;
		}
	}

	void RegisterPoseAssetRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace PoseMirror;

		Core.RegisterRoute(TEXT("/api/anim/pose_asset"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Anim/PA_Face")))
					{
						return;
					}
					FString AnimationPath, SkeletonSpec;
					Body->TryGetStringField(TEXT("animation"), AnimationPath);
					Body->TryGetStringField(TEXT("skeleton"), SkeletonSpec);
					UAnimSequence* Animation = nullptr;
					if (!AnimationPath.IsEmpty())
					{
						Animation = Cast<UAnimSequence>(ResolveAsset(AnimationPath));
						if (Animation == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
								FString::Printf(TEXT("no Animation Sequence at '%s'"), *AnimationPath));
							return;
						}
					}
					USkeleton* Skeleton = Animation != nullptr ? Animation->GetSkeleton()
						: (SkeletonSpec.IsEmpty() ? nullptr : SkeletonFromSpec(SkeletonSpec));
					if (Skeleton == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'skeleton' (a Skeleton, skeletal mesh or animation path) or 'animation' is required"));
						return;
					}
					TArray<FName> PoseNames;
					ReadNameList(Body, TEXT("pose_names"), PoseNames);
					FString ReferencePose;
					Body->TryGetStringField(TEXT("reference_pose"), ReferencePose);
					FName AssetName;
					UPackage* Package = PackageForNewAsset(Path, Responder, AssetName);
					if (Package == nullptr)
					{
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreatePoseAsset", "McpLink Create Pose Asset"));
					UPoseAsset* Asset = NewObject<UPoseAsset>(Package, AssetName,
						RF_Public | RF_Standalone | RF_Transactional);
					Asset->SetSkeleton(Skeleton);
					if (Animation != nullptr)
					{
						// One pose per frame, named as given or after the frames.
						Asset->CreatePoseFromAnimation(Animation, PoseNames.Num() > 0 ? &PoseNames : nullptr);
					}
					if (!ReferencePose.IsEmpty())
					{
						Asset->AddReferencePose(FName(*ReferencePose), Skeleton->GetReferenceSkeleton());
					}
					FAssetRegistryModule::AssetCreated(Asset);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = PoseAssetJson(*Asset, 200);
					Data->SetStringField(TEXT("message"),
						Animation != nullptr ? TEXT("created from the animation — one pose per frame")
											 : TEXT("created empty — add_reference_pose or update_from_animation fill it"));
					Responder->Ok(Data);
					return;
				}

				UPoseAsset* Asset = PoseAssetOrError(Body, Responder);
				if (Asset == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					Responder->Ok(PoseAssetJson(*Asset, FMath::Clamp(IntOr(Body, TEXT("max_names"), 200), 1, 5000)));
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
					Data->SetStringField(TEXT("pose_asset"), Asset->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditPoseAsset", "McpLink Edit Pose Asset"));
				Asset->Modify();

				if (Operation == TEXT("update_from_animation"))
				{
					FString AnimationPath;
					if (!RequireString(Body, TEXT("animation"), AnimationPath, Responder, TEXT("an Animation Sequence path")))
					{
						return;
					}
					UAnimSequence* Animation = Cast<UAnimSequence>(ResolveAsset(AnimationPath));
					if (Animation == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
							FString::Printf(TEXT("no Animation Sequence at '%s'"), *AnimationPath));
						return;
					}
					Asset->UpdatePoseFromAnimation(Animation);
					Responder->Ok(PoseAssetJson(*Asset, 200));
					return;
				}

				if (Operation == TEXT("add_reference_pose"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the new pose's name")))
					{
						return;
					}
					if (Asset->GetSkeleton() == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_skeleton"),
							TEXT("the Pose Asset has no skeleton"));
						return;
					}
					Asset->AddReferencePose(FName(*Name), Asset->GetSkeleton()->GetReferenceSkeleton());
					Responder->Ok(PoseAssetJson(*Asset, 200));
					return;
				}

				if (Operation == TEXT("rename_pose"))
				{
					FString Name, NewName;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the pose to rename"))
						|| !RequireString(Body, TEXT("new_name"), NewName, Responder, TEXT("its new name")))
					{
						return;
					}
					if (!Asset->ContainsPose(FName(*Name)))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pose_not_found"),
							FString::Printf(TEXT("no pose named '%s'"), *Name));
						return;
					}
					if (!Asset->ModifyPoseName(FName(*Name), FName(*NewName)))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("rename_refused"),
							FString::Printf(TEXT("could not rename '%s' to '%s' — is the name taken?"), *Name, *NewName));
						return;
					}
					Responder->Ok(PoseAssetJson(*Asset, 200));
					return;
				}

				if (Operation == TEXT("delete_poses") || Operation == TEXT("delete_curves"))
				{
					TArray<FName> Names;
					if (!ReadNameList(Body, TEXT("names"), Names) || Names.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'names' (a non-empty list) is required"));
						return;
					}
					const int32 Removed = Operation == TEXT("delete_poses")
						? Asset->DeletePoses(Names) : Asset->DeleteCurves(Names);
					const TSharedRef<FJsonObject> Data = PoseAssetJson(*Asset, 200);
					Data->SetNumberField(TEXT("removed"), Removed);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_additive"))
				{
					const bool bAdditive = BoolOr(Body, TEXT("additive"), true);
					int32 BaseIndex = IntOr(Body, TEXT("base_pose_index"), Asset->GetBasePoseIndex());
					FString BasePose;
					if (Body->TryGetStringField(TEXT("base_pose"), BasePose))
					{
						BaseIndex = Asset->GetPoseFNames().IndexOfByKey(FName(*BasePose));
						if (BaseIndex == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pose_not_found"),
								FString::Printf(TEXT("no pose named '%s'"), *BasePose));
							return;
						}
					}
					if (bAdditive && !Asset->GetPoseFNames().IsValidIndex(BaseIndex))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_base_pose"),
							TEXT("an additive Pose Asset needs 'base_pose' (a pose name) or 'base_pose_index'; ")
							TEXT("-1 means the reference pose"));
						return;
					}
					if (!Asset->ConvertSpace(bAdditive, BaseIndex))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("convert_refused"),
							TEXT("the Pose Asset refused the conversion — is it already in that space?"));
						return;
					}
					Responder->Ok(PoseAssetJson(*Asset, 200));
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected create, info, update_from_animation, ")
						TEXT("add_reference_pose, rename_pose, delete_poses, delete_curves, set_additive or save"),
						*Operation));
			});
	}

	void RegisterMirrorTableRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace PoseMirror;

		Core.RegisterRoute(TEXT("/api/anim/mirror_table"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path, SkeletonSpec;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Anim/MDT_Hero"))
						|| !RequireString(Body, TEXT("skeleton"), SkeletonSpec, Responder,
							TEXT("the Skeleton (or a skeletal mesh / animation using it)")))
					{
						return;
					}
					USkeleton* Skeleton = SkeletonFromSpec(SkeletonSpec);
					if (Skeleton == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("skeleton_not_found"),
							FString::Printf(TEXT("no Skeleton at '%s'"), *SkeletonSpec));
						return;
					}
					EAxis::Type Axis = EAxis::X;
					FString Error;
					if (!ReadAxis(Body, Axis, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_axis"), Error);
						return;
					}
					TArray<FMirrorFindReplaceExpression> Expressions;
					if (Body->HasField(TEXT("expressions")) && !ReadExpressions(Body, Expressions, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_expressions"), Error);
						return;
					}
					if (Expressions.IsEmpty())
					{
						// The project's defaults: the same list the New Mirror
						// Data Table dialog starts from (Left/Right, _l/_r, …).
						Expressions = GetDefault<UAnimationSettings>()->MirrorFindReplaceExpressions;
					}
					FName AssetName;
					UPackage* Package = PackageForNewAsset(Path, Responder, AssetName);
					if (Package == nullptr)
					{
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMirrorTable", "McpLink Create Mirror Data Table"));
					UMirrorDataTable* Table = NewObject<UMirrorDataTable>(Package, AssetName,
						RF_Public | RF_Standalone | RF_Transactional);
					Table->RowStruct = FMirrorTableRow::StaticStruct();
					Table->Skeleton = Skeleton;
					Table->MirrorAxis = Axis;
					Table->MirrorFindReplaceExpressions = Expressions;
					// Run the expressions over every bone, notify, curve and
					// sync marker of the skeleton, which is what the factory does.
					Table->UpdateFromFindReplaceExpressions(UMirrorDataTable::FFindReplaceOptions::Sync());
					FAssetRegistryModule::AssetCreated(Table);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MirrorTableJson(*Table, 200);
					Data->SetStringField(TEXT("message"),
						TEXT("created and synced — rows are editable with data_table_ops; re-run sync after adding expressions"));
					Responder->Ok(Data);
					return;
				}

				UMirrorDataTable* Table = MirrorTableOrError(Body, Responder);
				if (Table == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					Responder->Ok(MirrorTableJson(*Table, FMath::Clamp(IntOr(Body, TEXT("max_rows"), 200), 1, 5000)));
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Table, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("table"), Table->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditMirrorTable", "McpLink Edit Mirror Data Table"));
				Table->Modify();

				if (Operation == TEXT("sync"))
				{
					FString Mode = TEXT("sync");
					Body->TryGetStringField(TEXT("mode"), Mode);
					UMirrorDataTable::FFindReplaceOptions Options = UMirrorDataTable::FFindReplaceOptions::Sync();
					if (Mode == TEXT("add_missing"))
					{
						Options = UMirrorDataTable::FFindReplaceOptions::AddMissingOnly();
					}
					else if (Mode == TEXT("update_existing"))
					{
						Options = UMirrorDataTable::FFindReplaceOptions::UpdateExisting();
					}
					else if (Mode != TEXT("sync"))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_mode"),
							TEXT("'mode' is sync (default: rebuild every row from the expressions), ")
							TEXT("add_missing (keep edited rows, add new names) or update_existing"));
						return;
					}
					if (Table->Skeleton == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_skeleton"),
							TEXT("the table has no skeleton to sync against"));
						return;
					}
					Table->UpdateFromFindReplaceExpressions(Options);
					const TSharedRef<FJsonObject> Data = MirrorTableJson(*Table, 200);
					Data->SetStringField(TEXT("mode"), Mode);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_axis"))
				{
					EAxis::Type Axis = Table->MirrorAxis;
					FString Error;
					if (!Body->HasField(TEXT("axis")) || !ReadAxis(Body, Axis, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_axis"),
							Error.IsEmpty() ? TEXT("'axis' (X, Y or Z) is required") : *Error);
						return;
					}
					Table->MirrorAxis = Axis;
					Table->PostEditChange();
					Responder->Ok(MirrorTableJson(*Table, 200));
					return;
				}

				if (Operation == TEXT("set_expressions"))
				{
					TArray<FMirrorFindReplaceExpression> Expressions;
					FString Error;
					if (!ReadExpressions(Body, Expressions, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_expressions"),
							Error.IsEmpty() ? TEXT("'expressions' (a list of {find, replace, method?}) is required") : *Error);
						return;
					}
					Table->MirrorFindReplaceExpressions = Expressions;
					if (BoolOr(Body, TEXT("sync"), true) && Table->Skeleton != nullptr)
					{
						Table->UpdateFromFindReplaceExpressions(UMirrorDataTable::FFindReplaceOptions::Sync());
					}
					Responder->Ok(MirrorTableJson(*Table, 200));
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected create, info, sync, set_axis, ")
						TEXT("set_expressions or save (rows: data_table_ops)"), *Operation));
			});
	}
}
