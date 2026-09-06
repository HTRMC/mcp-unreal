// IK Rig and IK Retargeter: rigs (skeleton, solvers, goals, retarget root
// and chains), retargeters (source and target rigs, chain mapping,
// settings) and the batch that retargets animations between skeletons.

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
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/IKRigProcessor.h"
#include "RigEditor/IKRigController.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace IkRigs
	{
		UPackage* NewAssetPackageOrError(const FString& Path, const TSharedRef<FMcpResponder>& Responder, FName& OutName)
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

		UIKRigDefinition* RigOrError(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, Field, Path, Responder, TEXT("an IK Rig asset path")))
			{
				return nullptr;
			}
			UIKRigDefinition* Rig = Cast<UIKRigDefinition>(ResolveAsset(Path));
			if (Rig == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("rig_not_found"),
					FString::Printf(TEXT("no IK Rig at '%s'"), *Path));
			}
			return Rig;
		}

		UIKRetargeter* RetargeterOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("retargeter"), Path, Responder, TEXT("an IK Retargeter asset path")))
			{
				return nullptr;
			}
			UIKRetargeter* Retargeter = Cast<UIKRetargeter>(ResolveAsset(Path));
			if (Retargeter == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("retargeter_not_found"),
					FString::Printf(TEXT("no IK Retargeter at '%s'"), *Path));
			}
			return Retargeter;
		}

		UScriptStruct* SolverTypeOrError(const FString& Spec, const TSharedRef<FMcpResponder>& Responder)
		{
			UScriptStruct* Base = FIKRigSolverBase::StaticStruct();
			TArray<FString> Names;
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				if (*It == Base || !It->IsChildOf(Base))
				{
					continue;
				}
				const FString Name = It->GetName();
				Names.Add(Name);
				if (Name == Spec || It->GetPathName() == Spec || (TEXT("F") + Name) == Spec
					|| Name.Equals(Spec, ESearchCase::IgnoreCase))
				{
					return *It;
				}
			}
			Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("solver_not_found"),
				FString::Printf(TEXT("'%s' is not an IK Rig solver type — one of: %s"), *Spec, *FString::Join(Names, TEXT(", "))));
			return nullptr;
		}

		TSharedRef<FJsonObject> RigJson(UIKRigDefinition& Rig, int32 MaxBones)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("rig"), Rig.GetPathName());
			Data->SetStringField(TEXT("skeletal_mesh"), Rig.GetPreviewMesh() != nullptr ? Rig.GetPreviewMesh()->GetPathName() : FString());
			Data->SetStringField(TEXT("retarget_root"), Rig.GetRoot().ToString());
			const TArray<FName>& Bones = Rig.GetSkeleton().BoneNames;
			Data->SetNumberField(TEXT("bone_count"), Bones.Num());
			TArray<TSharedPtr<FJsonValue>> BoneJson;
			for (int32 Index = 0; Index < Bones.Num() && Index < MaxBones; ++Index)
			{
				BoneJson.Add(MakeShared<FJsonValueString>(Bones[Index].ToString()));
			}
			Data->SetArrayField(TEXT("bones"), BoneJson);
			TArray<TSharedPtr<FJsonValue>> Solvers;
			if (const UIKRigController* Controller = UIKRigController::GetController(&Rig))
			{
				for (int32 Index = 0; Index < Controller->GetNumSolvers(); ++Index)
				{
					const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetNumberField(TEXT("index"), Index);
					Entry->SetStringField(TEXT("name"), Controller->GetSolverUniqueName(Index));
					if (const FIKRigSolverBase* Solver = Controller->GetSolverAtIndex(Index))
					{
						Entry->SetBoolField(TEXT("enabled"), Solver->IsEnabled());
					}
					Solvers.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			Data->SetArrayField(TEXT("solvers"), Solvers);
			TArray<TSharedPtr<FJsonValue>> Goals;
			for (const UIKRigEffectorGoal* Goal : Rig.GetGoalArray())
			{
				if (Goal == nullptr)
				{
					continue;
				}
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Goal->GoalName.ToString());
				Entry->SetStringField(TEXT("bone"), Goal->BoneName.ToString());
				Goals.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("goals"), Goals);
			TArray<TSharedPtr<FJsonValue>> Chains;
			for (const FBoneChain& Chain : Rig.GetRetargetChains())
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Chain.ChainName.ToString());
				Entry->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
				Entry->SetStringField(TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
				Entry->SetStringField(TEXT("goal"), Chain.IKGoalName.ToString());
				Chains.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("chains"), Chains);
			return Data;
		}

		TSharedRef<FJsonObject> RetargeterJson(UIKRetargeter& Retargeter)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("retargeter"), Retargeter.GetPathName());
			const UIKRigDefinition* Source = Retargeter.GetIKRig(ERetargetSourceOrTarget::Source);
			const UIKRigDefinition* Target = Retargeter.GetIKRig(ERetargetSourceOrTarget::Target);
			Data->SetStringField(TEXT("source_rig"), Source != nullptr ? Source->GetPathName() : FString());
			Data->SetStringField(TEXT("target_rig"), Target != nullptr ? Target->GetPathName() : FString());
			const UIKRetargeterController* Controller = UIKRetargeterController::GetController(&Retargeter);
			if (Controller != nullptr)
			{
				USkeletalMesh* SourceMesh = Controller->GetPreviewMesh(ERetargetSourceOrTarget::Source);
				USkeletalMesh* TargetMesh = Controller->GetPreviewMesh(ERetargetSourceOrTarget::Target);
				Data->SetStringField(TEXT("source_mesh"), SourceMesh != nullptr ? SourceMesh->GetPathName() : FString());
				Data->SetStringField(TEXT("target_mesh"), TargetMesh != nullptr ? TargetMesh->GetPathName() : FString());
				Data->SetStringField(TEXT("source_pose"), Controller->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Source).ToString());
				Data->SetStringField(TEXT("target_pose"), Controller->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target).ToString());
			}
			if (Controller != nullptr)
			{
				Data->SetNumberField(TEXT("ops"), Controller->GetNumRetargetOps());
			}
			TArray<TSharedPtr<FJsonValue>> Mapping;
			if (Target != nullptr && Controller != nullptr)
			{
				for (const FBoneChain& Chain : Target->GetRetargetChains())
				{
					const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("target_chain"), Chain.ChainName.ToString());
					Entry->SetStringField(TEXT("source_chain"), Controller->GetSourceChain(Chain.ChainName).ToString());
					Mapping.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			Data->SetArrayField(TEXT("chain_mapping"), Mapping);
			return Data;
		}
	}

	void RegisterIkRigRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace IkRigs;

		Core.RegisterRoute(TEXT("/api/anim/ik_rig"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_solver_types"))
				{
					TArray<TSharedPtr<FJsonValue>> Types;
					for (TObjectIterator<UScriptStruct> It; It; ++It)
					{
						if (*It != FIKRigSolverBase::StaticStruct() && It->IsChildOf(FIKRigSolverBase::StaticStruct()))
						{
							Types.Add(MakeShared<FJsonValueString>(It->GetName()));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("solver_types"), Types);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_rig"))
				{
					FString Path, MeshPath;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Rigs/IK_Hero"))
						|| !RequireString(Body, TEXT("skeletal_mesh"), MeshPath, Responder, TEXT("the Skeletal Mesh the rig is for")))
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
					FName AssetName;
					UPackage* Package = NewAssetPackageOrError(Path, Responder, AssetName);
					if (Package == nullptr)
					{
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "CreateIkRig", "McpLink Create IK Rig"));
					UIKRigDefinition* Rig = NewObject<UIKRigDefinition>(Package, AssetName, RF_Public | RF_Standalone | RF_Transactional);
					UIKRigController* Controller = UIKRigController::GetController(Rig);
					// The skeleton is copied out of the mesh; the rig editor does
					// the same when a mesh is dropped on a new rig.
					if (Controller == nullptr || !Controller->SetSkeletalMesh(Mesh))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("mesh_refused"),
							TEXT("the rig controller refused the skeletal mesh"));
						return;
					}
					bool bAutoRetarget = false;
					if (BoolOr(Body, TEXT("auto_retarget"), false))
					{
						bAutoRetarget = Controller->ApplyAutoGeneratedRetargetDefinition();
					}
					FAssetRegistryModule::AssetCreated(Rig);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = RigJson(*Rig, 50);
					Data->SetBoolField(TEXT("auto_retarget_applied"), bAutoRetarget);
					Data->SetStringField(TEXT("message"),
						TEXT("created — auto_retarget characterizes a humanoid (root + chains), add_solver / add_goal / add_chain do it by hand"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_retargeter"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Rigs/RTG_AToB")))
					{
						return;
					}
					UIKRigDefinition* Source = RigOrError(Body, TEXT("source_rig"), Responder);
					if (Source == nullptr)
					{
						return;
					}
					UIKRigDefinition* Target = RigOrError(Body, TEXT("target_rig"), Responder);
					if (Target == nullptr)
					{
						return;
					}
					FName AssetName;
					UPackage* Package = NewAssetPackageOrError(Path, Responder, AssetName);
					if (Package == nullptr)
					{
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "CreateRetargeter", "McpLink Create IK Retargeter"));
					UIKRetargeter* Retargeter = NewObject<UIKRetargeter>(Package, AssetName, RF_Public | RF_Standalone | RF_Transactional);
					UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
					Controller->SetIKRig(ERetargetSourceOrTarget::Source, Source);
					Controller->SetIKRig(ERetargetSourceOrTarget::Target, Target);
					// 5.8 retargets through an op stack (root, FK chains, IK
					// chains, ...); the chain mapping lives on those ops, so the
					// New IK Retargeter defaults come first, then the pairing by
					// closest name.
					Controller->AddDefaultOps();
					Controller->AutoMapChains(EAutoMapChainType::Fuzzy, true);
					FAssetRegistryModule::AssetCreated(Retargeter);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = RetargeterJson(*Retargeter);
					Data->SetStringField(TEXT("message"),
						TEXT("created with fuzzy chain mapping — map_chain corrects pairs, retarget_animations runs the batch"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("retargeter_info") || Operation == TEXT("map_chain") || Operation == TEXT("auto_map")
					|| Operation == TEXT("auto_align") || Operation == TEXT("save_retargeter") || Operation == TEXT("retarget_animations"))
				{
					UIKRetargeter* Retargeter = RetargeterOrError(Body, Responder);
					if (Retargeter == nullptr)
					{
						return;
					}
					UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);

					if (Operation == TEXT("retargeter_info"))
					{
						Responder->Ok(RetargeterJson(*Retargeter));
						return;
					}

					if (Operation == TEXT("save_retargeter"))
					{
						FString Filename, Error;
						if (!SaveAsset(Retargeter, Filename, Error))
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
							return;
						}
						const TSharedRef<FJsonObject> Data = RetargeterJson(*Retargeter);
						Data->SetStringField(TEXT("file"), Filename);
						Responder->Ok(Data);
						return;
					}

					if (Operation == TEXT("retarget_animations"))
					{
						const TArray<TSharedPtr<FJsonValue>>* AnimValues = nullptr;
						if (!Body->TryGetArrayField(TEXT("animations"), AnimValues) || AnimValues->IsEmpty())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'animations' (Animation Sequence, Montage or Blend Space paths) is required"));
							return;
						}
						FIKRetargetBatchOperationInputs Inputs;
						for (const TSharedPtr<FJsonValue>& Value : *AnimValues)
						{
							FString Path;
							UObject* Asset = Value->TryGetString(Path) ? ResolveAsset(Path) : nullptr;
							if (Asset == nullptr)
							{
								Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
									FString::Printf(TEXT("no animation asset at '%s'"), *Path));
								return;
							}
							Inputs.AssetsToRetarget.Add(FAssetData(Asset));
						}
						Inputs.SourceMesh = Controller->GetPreviewMesh(ERetargetSourceOrTarget::Source);
						Inputs.TargetMesh = Controller->GetPreviewMesh(ERetargetSourceOrTarget::Target);
						FString MeshPath;
						if (Body->TryGetStringField(TEXT("source_mesh"), MeshPath) && !MeshPath.IsEmpty())
						{
							Inputs.SourceMesh = Cast<USkeletalMesh>(ResolveAsset(MeshPath));
						}
						if (Body->TryGetStringField(TEXT("target_mesh"), MeshPath) && !MeshPath.IsEmpty())
						{
							Inputs.TargetMesh = Cast<USkeletalMesh>(ResolveAsset(MeshPath));
						}
						if (Inputs.SourceMesh == nullptr || Inputs.TargetMesh == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_meshes"),
								TEXT("the retargeter's rigs have no skeletal meshes — pass 'source_mesh' and 'target_mesh'"));
							return;
						}
						Inputs.IKRetargetAsset = Retargeter;
						Body->TryGetStringField(TEXT("search"), Inputs.Search);
						Body->TryGetStringField(TEXT("replace"), Inputs.Replace);
						Body->TryGetStringField(TEXT("prefix"), Inputs.Prefix);
						Body->TryGetStringField(TEXT("suffix"), Inputs.Suffix);
						Body->TryGetStringField(TEXT("target_folder"), Inputs.TargetPath);
						Inputs.bUseSourcePath = Inputs.TargetPath.IsEmpty();
						Inputs.bIncludeReferencedAssets = BoolOr(Body, TEXT("include_referenced"), true);
						Inputs.bOverwriteExistingFiles = BoolOr(Body, TEXT("overwrite"), false);
						Inputs.bRetainAdditiveFlags = BoolOr(Body, TEXT("retain_additive"), true);
						if (Inputs.Prefix.IsEmpty() && Inputs.Suffix.IsEmpty() && Inputs.Search.IsEmpty() && Inputs.TargetPath.IsEmpty())
						{
							// Something has to distinguish the copies from the originals.
							Inputs.Suffix = TEXT("_Retargeted");
						}
						// The Retarget Animations window's batch: duplicate each
						// asset next to its source (or into target_folder) and
						// bake the retargeted pose into the copy.
						const TArray<FAssetData> Created = UIKRetargetBatchOperation::RunBatchRetarget(Inputs);
						TArray<TSharedPtr<FJsonValue>> Assets;
						for (const FAssetData& Asset : Created)
						{
							Assets.Add(MakeShared<FJsonValueString>(Asset.GetObjectPathString()));
						}
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("retargeter"), Retargeter->GetPathName());
						Data->SetNumberField(TEXT("requested"), Inputs.AssetsToRetarget.Num());
						Data->SetNumberField(TEXT("created"), Assets.Num());
						Data->SetArrayField(TEXT("assets"), Assets);
						if (Assets.IsEmpty())
						{
							Data->SetStringField(TEXT("note"),
								TEXT("nothing was created — the log says why (a chain mapping or the retarget poses are the usual cause)"));
						}
						Responder->Ok(Data);
						return;
					}

					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "EditRetargeter", "McpLink Edit IK Retargeter"));
					Retargeter->Modify();
					if (Operation == TEXT("map_chain"))
					{
						FString TargetChain, SourceChain;
						if (!RequireString(Body, TEXT("target_chain"), TargetChain, Responder, TEXT("a chain of the target rig")))
						{
							return;
						}
						Body->TryGetStringField(TEXT("source_chain"), SourceChain);
						if (!Controller->SetSourceChain(SourceChain.IsEmpty() ? NAME_None : FName(*SourceChain), FName(*TargetChain)))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("chain_not_found"),
								FString::Printf(TEXT("could not map '%s' to '%s' — retargeter_info lists both rigs' chains"),
									*TargetChain, *SourceChain));
							return;
						}
					}
					else if (Operation == TEXT("auto_map"))
					{
						FString Method = TEXT("fuzzy");
						Body->TryGetStringField(TEXT("method"), Method);
						const EAutoMapChainType Type = Method == TEXT("exact") ? EAutoMapChainType::Exact
							: (Method == TEXT("clear") ? EAutoMapChainType::Clear : EAutoMapChainType::Fuzzy);
						Controller->AutoMapChains(Type, true);
					}
					else if (Operation == TEXT("auto_align"))
					{
						FString Which = TEXT("target");
						Body->TryGetStringField(TEXT("which"), Which);
						Controller->AutoAlignAllBones(Which == TEXT("source") ? ERetargetSourceOrTarget::Source : ERetargetSourceOrTarget::Target);
					}
					Retargeter->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = RetargeterJson(*Retargeter);
					Data->SetStringField(TEXT("applied"), Operation);
					Responder->Ok(Data);
					return;
				}

				UIKRigDefinition* Rig = RigOrError(Body, TEXT("rig"), Responder);
				if (Rig == nullptr)
				{
					return;
				}
				UIKRigController* Controller = UIKRigController::GetController(Rig);
				if (Controller == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_controller"), TEXT("the rig has no controller"));
					return;
				}

				if (Operation == TEXT("rig_info"))
				{
					Responder->Ok(RigJson(*Rig, FMath::Clamp(IntOr(Body, TEXT("max_bones"), 200), 0, 5000)));
					return;
				}

				if (Operation == TEXT("save_rig"))
				{
					FString Filename, Error;
					if (!SaveAsset(Rig, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = RigJson(*Rig, 0);
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "EditIkRig", "McpLink Edit IK Rig"));
				Rig->Modify();

				if (Operation == TEXT("auto_retarget"))
				{
					const bool bApplied = Controller->ApplyAutoGeneratedRetargetDefinition();
					Rig->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = RigJson(*Rig, 0);
					Data->SetBoolField(TEXT("applied"), bApplied);
					if (!bApplied)
					{
						Data->SetStringField(TEXT("note"),
							TEXT("the skeleton was not recognised as a known humanoid — set_retarget_root and add_chain by hand"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_solver"))
				{
					FString TypeSpec;
					if (!RequireString(Body, TEXT("solver"), TypeSpec, Responder, TEXT("a solver type from list_solver_types, e.g. IKRigFullBodyIKSolver")))
					{
						return;
					}
					UScriptStruct* Type = SolverTypeOrError(TypeSpec, Responder);
					if (Type == nullptr)
					{
						return;
					}
					const int32 Index = Controller->AddSolver(Type);
					if (Index == INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("solver_refused"), TEXT("the rig refused the solver"));
						return;
					}
					FString RootBone;
					if (Body->TryGetStringField(TEXT("root_bone"), RootBone) && !RootBone.IsEmpty())
					{
						Controller->SetStartBone(FName(*RootBone), Index);
					}
					FString EndBone;
					if (Body->TryGetStringField(TEXT("end_bone"), EndBone) && !EndBone.IsEmpty())
					{
						Controller->SetEndBone(FName(*EndBone), Index);
					}
					Rig->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = RigJson(*Rig, 0);
					Data->SetNumberField(TEXT("solver_index"), Index);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_solver_enabled"))
				{
					const int32 Index = IntOr(Body, TEXT("solver"), -1);
					if (!Controller->SetSolverEnabled(Index, BoolOr(Body, TEXT("enabled"), true)))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("solver_not_found"),
							FString::Printf(TEXT("no solver at index %d"), Index));
						return;
					}
					Rig->MarkPackageDirty();
					Responder->Ok(RigJson(*Rig, 0));
					return;
				}

				if (Operation == TEXT("add_goal"))
				{
					FString GoalName, BoneName;
					if (!RequireString(Body, TEXT("name"), GoalName, Responder, TEXT("the goal's name"))
						|| !RequireString(Body, TEXT("bone"), BoneName, Responder, TEXT("the bone the goal drives")))
					{
						return;
					}
					const FName Added = Controller->AddNewGoal(FName(*GoalName), FName(*BoneName));
					if (Added.IsNone())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("goal_refused"),
							FString::Printf(TEXT("the rig refused goal '%s' on bone '%s' — is the bone in the skeleton, or the goal name taken?"),
								*GoalName, *BoneName));
						return;
					}
					const int32 Solver = IntOr(Body, TEXT("solver"), -1);
					bool bConnected = false;
					if (Solver >= 0)
					{
						bConnected = Controller->ConnectGoalToSolver(Added, Solver);
					}
					Rig->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = RigJson(*Rig, 0);
					Data->SetStringField(TEXT("goal"), Added.ToString());
					Data->SetBoolField(TEXT("connected"), bConnected);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_retarget_root"))
				{
					FString BoneName;
					if (!RequireString(Body, TEXT("bone"), BoneName, Responder, TEXT("the retarget root bone, usually the pelvis")))
					{
						return;
					}
					if (!Controller->SetRetargetRoot(FName(*BoneName)))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("bone_not_found"),
							FString::Printf(TEXT("no bone '%s' in the rig's skeleton"), *BoneName));
						return;
					}
					Rig->MarkPackageDirty();
					Responder->Ok(RigJson(*Rig, 0));
					return;
				}

				if (Operation == TEXT("add_chain"))
				{
					FString ChainName, StartBone, EndBone, Goal;
					if (!RequireString(Body, TEXT("name"), ChainName, Responder, TEXT("the chain's name, e.g. LeftArm"))
						|| !RequireString(Body, TEXT("start_bone"), StartBone, Responder, TEXT("the chain's first bone"))
						|| !RequireString(Body, TEXT("end_bone"), EndBone, Responder, TEXT("the chain's last bone")))
					{
						return;
					}
					Body->TryGetStringField(TEXT("goal"), Goal);
					const FName Added = Controller->AddRetargetChain(FName(*ChainName), FName(*StartBone), FName(*EndBone),
						Goal.IsEmpty() ? NAME_None : FName(*Goal));
					if (Added.IsNone())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("chain_refused"),
							TEXT("the rig refused the chain — both bones must exist and the end must descend from the start"));
						return;
					}
					Rig->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = RigJson(*Rig, 0);
					Data->SetStringField(TEXT("chain"), Added.ToString());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_chain"))
				{
					FString ChainName;
					if (!RequireString(Body, TEXT("name"), ChainName, Responder, TEXT("a chain name from rig_info")))
					{
						return;
					}
					if (!Controller->RemoveRetargetChain(FName(*ChainName)))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("chain_not_found"),
							FString::Printf(TEXT("no chain '%s'"), *ChainName));
						return;
					}
					Rig->MarkPackageDirty();
					Responder->Ok(RigJson(*Rig, 0));
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected list_solver_types, create_rig, rig_info, auto_retarget, ")
						TEXT("add_solver, set_solver_enabled, add_goal, set_retarget_root, add_chain, remove_chain, save_rig, ")
						TEXT("create_retargeter, retargeter_info, map_chain, auto_map, auto_align, retarget_animations or save_retargeter"),
						*Operation));
			});
	}
}
