// Control Rig: the rig Blueprint's hierarchy of bones, nulls and controls,
// and the Sequencer track that drives a rig on a bound actor.

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "ControlRig.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigBlueprintLegacy.h"
#include "ControlRigObjectBinding.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyElements.h"
#include "ScopedTransaction.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace ControlRigs
	{
		UControlRigBlueprint* RigOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("rig"), Path, Responder, TEXT("a Control Rig Blueprint asset path")))
			{
				return nullptr;
			}
			UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(ResolveAsset(Path));
			if (Rig == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("rig_not_found"),
					FString::Printf(TEXT("no Control Rig Blueprint at '%s'"), *Path));
			}
			return Rig;
		}

		bool ReadTransform(const TSharedPtr<FJsonObject>& Object, FTransform& Out)
		{
			Out = FTransform::Identity;
			if (!Object.IsValid())
			{
				return false;
			}
			const TSharedRef<FJsonObject> Ref = Object.ToSharedRef();
			FVector Location;
			if (GetVector(Ref, TEXT("location"), Location))
			{
				Out.SetLocation(Location);
			}
			FRotator Rotation;
			if (GetRotator(Ref, TEXT("rotation"), Rotation))
			{
				Out.SetRotation(Rotation.Quaternion());
			}
			FVector Scale;
			if (GetVector(Ref, TEXT("scale"), Scale))
			{
				Out.SetScale3D(Scale);
			}
			return true;
		}

		TSharedRef<FJsonObject> TransformJson(const FTransform& Transform)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("location"), VectorToJson(Transform.GetLocation()));
			Data->SetArrayField(TEXT("rotation"), RotatorToJson(Transform.Rotator()));
			Data->SetArrayField(TEXT("scale"), VectorToJson(Transform.GetScale3D()));
			return Data;
		}

		bool ReadNumbers(const TSharedPtr<FJsonValue>& Value, int32 Count, double* Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Value.IsValid() || !Value->TryGetArray(Items) || Items->Num() != Count)
			{
				return false;
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Out[Index] = (*Items)[Index]->AsNumber();
			}
			return true;
		}

		/// A JSON value in the shape of the control type — number, bool,
		/// [x, y], [x, y, z] or a transform object — as a rig value.
		bool MakeControlValue(ERigControlType Type, const TSharedPtr<FJsonValue>& Value, FRigControlValue& Out, FString& Error)
		{
			double Numbers[3] = {0.0, 0.0, 0.0};
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FTransform Transform;
			switch (Type)
			{
			case ERigControlType::Bool:
			{
				bool bValue = false;
				if (!Value.IsValid() || !Value->TryGetBool(bValue))
				{
					Error = TEXT("a Bool control takes true or false");
					return false;
				}
				Out = FRigControlValue::Make<bool>(bValue);
				return true;
			}
			case ERigControlType::Float:
			case ERigControlType::ScaleFloat:
			{
				double Number = 0.0;
				if (!Value.IsValid() || !Value->TryGetNumber(Number))
				{
					Error = TEXT("a Float control takes a number");
					return false;
				}
				Out = FRigControlValue::Make<float>(static_cast<float>(Number));
				return true;
			}
			case ERigControlType::Integer:
			{
				double Number = 0.0;
				if (!Value.IsValid() || !Value->TryGetNumber(Number))
				{
					Error = TEXT("an Integer control takes a number");
					return false;
				}
				Out = FRigControlValue::Make<int32>(static_cast<int32>(Number));
				return true;
			}
			case ERigControlType::Vector2D:
				if (!ReadNumbers(Value, 2, Numbers))
				{
					Error = TEXT("a Vector2D control takes [x, y]");
					return false;
				}
				Out = FRigControlValue::Make<FVector2D>(FVector2D(Numbers[0], Numbers[1]));
				return true;
			case ERigControlType::Position:
			case ERigControlType::Scale:
				if (!ReadNumbers(Value, 3, Numbers))
				{
					Error = TEXT("a Position or Scale control takes [x, y, z]");
					return false;
				}
				Out = FRigControlValue::Make<FVector3f>(FVector3f(
					static_cast<float>(Numbers[0]), static_cast<float>(Numbers[1]), static_cast<float>(Numbers[2])));
				return true;
			case ERigControlType::Rotator:
				if (!ReadNumbers(Value, 3, Numbers))
				{
					Error = TEXT("a Rotator control takes [pitch, yaw, roll]");
					return false;
				}
				Out = FRigControlValue::Make<FRotator>(FRotator(Numbers[0], Numbers[1], Numbers[2]));
				return true;
			case ERigControlType::Transform:
			case ERigControlType::TransformNoScale:
			case ERigControlType::EulerTransform:
				if (!Value.IsValid() || !Value->TryGetObject(Object) || !ReadTransform(*Object, Transform))
				{
					Error = TEXT("a Transform control takes {location, rotation, scale}");
					return false;
				}
				if (Type == ERigControlType::Transform)
				{
					Out = FRigControlValue::Make<FTransform>(Transform);
				}
				else if (Type == ERigControlType::TransformNoScale)
				{
					Out = FRigControlValue::Make<FTransformNoScale>(FTransformNoScale(Transform));
				}
				else
				{
					Out = FRigControlValue::Make<FEulerTransform>(FEulerTransform(Transform));
				}
				return true;
			default:
				Error = TEXT("unsupported control type");
				return false;
			}
		}

		TSharedPtr<FJsonValue> ControlValueJson(const FRigControlElement& Control, const FRigControlValue& Value)
		{
			switch (Control.Settings.ControlType)
			{
			case ERigControlType::Bool:
				return MakeShared<FJsonValueBoolean>(Value.Get<bool>());
			case ERigControlType::Float:
			case ERigControlType::ScaleFloat:
				return MakeShared<FJsonValueNumber>(Value.Get<float>());
			case ERigControlType::Integer:
				return MakeShared<FJsonValueNumber>(Value.Get<int32>());
			case ERigControlType::Vector2D:
			{
				const FVector3f V = Value.Get<FVector3f>();
				return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y)});
			}
			case ERigControlType::Position:
			case ERigControlType::Scale:
			{
				const FVector3f V = Value.Get<FVector3f>();
				return MakeShared<FJsonValueArray>(VectorToJson(FVector(V)));
			}
			case ERigControlType::Rotator:
			{
				const FVector3f V = Value.Get<FVector3f>();
				return MakeShared<FJsonValueArray>(RotatorToJson(FRotator(V.X, V.Y, V.Z)));
			}
			default:
				return MakeShared<FJsonValueObject>(TransformJson(
					Value.GetAsTransform(Control.Settings.ControlType, Control.Settings.PrimaryAxis)));
			}
		}

		FRigElementKey ParentKeyOrError(URigHierarchy& Hierarchy, const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder, bool& bOutFailed)
		{
			bOutFailed = false;
			FString Parent;
			if (!Body->TryGetStringField(TEXT("parent"), Parent) || Parent.IsEmpty())
			{
				return FRigElementKey();
			}
			// Any element type with that name; bones first, then nulls, then controls.
			for (const ERigElementType Type : {ERigElementType::Bone, ERigElementType::Null, ERigElementType::Control})
			{
				const FRigElementKey Key(FName(*Parent), Type);
				if (Hierarchy.GetIndex(Key) != INDEX_NONE)
				{
					return Key;
				}
			}
			Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("parent_not_found"),
				FString::Printf(TEXT("no bone, null or control named '%s' — info lists them"), *Parent));
			bOutFailed = true;
			return FRigElementKey();
		}

		TSharedRef<FJsonObject> HierarchyJson(UControlRigBlueprint& Rig, int32 MaxElements)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("rig"), Rig.GetPathName());
			Data->SetStringField(TEXT("rig_class"), Rig.GetControlRigClass() != nullptr ? Rig.GetControlRigClass()->GetPathName() : FString());
			Data->SetStringField(TEXT("preview_mesh"), Rig.GetPreviewMesh() != nullptr ? Rig.GetPreviewMesh()->GetPathName() : FString());
			URigHierarchy* Hierarchy = Rig.GetHierarchy();
			TArray<TSharedPtr<FJsonValue>> Elements;
			int32 Bones = 0, Nulls = 0, Controls = 0, Total = 0;
			if (Hierarchy != nullptr)
			{
				for (const FRigElementKey& Key : Hierarchy->GetAllKeys(true))
				{
					if (Key.Type == ERigElementType::Bone) { ++Bones; }
					else if (Key.Type == ERigElementType::Null) { ++Nulls; }
					else if (Key.Type == ERigElementType::Control) { ++Controls; }
					else { continue; }
					++Total;
					if (Elements.Num() >= MaxElements)
					{
						continue;
					}
					const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), Key.Name.ToString());
					Entry->SetStringField(TEXT("type"), StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Key.Type)));
					const FRigElementKey Parent = Hierarchy->GetFirstParent(Key);
					Entry->SetStringField(TEXT("parent"), Parent.IsValid() ? Parent.Name.ToString() : FString());
					if (FRigTransformElement* TransformElement = Hierarchy->Find<FRigTransformElement>(Key))
					{
						Entry->SetObjectField(TEXT("global"),
							TransformJson(Hierarchy->GetTransform(TransformElement, ERigTransformType::CurrentGlobal)));
					}
					if (FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key))
					{
						Entry->SetStringField(TEXT("control_type"),
							StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(Control->Settings.ControlType)));
						Entry->SetStringField(TEXT("display_name"), Control->Settings.DisplayName.ToString());
						Entry->SetBoolField(TEXT("shape_visible"), Control->Settings.bShapeVisible);
						Entry->SetStringField(TEXT("shape"), Control->Settings.ShapeName.ToString());
						Entry->SetField(TEXT("value"), ControlValueJson(*Control,
							Hierarchy->GetControlValue(Control, ERigControlValueType::Current)));
					}
					Elements.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			Data->SetNumberField(TEXT("bones"), Bones);
			Data->SetNumberField(TEXT("nulls"), Nulls);
			Data->SetNumberField(TEXT("controls"), Controls);
			Data->SetNumberField(TEXT("total"), Total);
			Data->SetArrayField(TEXT("elements"), Elements);
			return Data;
		}

		UMovieSceneSequence* SequenceOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("sequence"), Path, Responder, TEXT("a Level Sequence asset path")))
			{
				return nullptr;
			}
			UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(ResolveAsset(Path));
			if (Sequence == nullptr || Sequence->GetMovieScene() == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("sequence_not_found"),
					FString::Printf(TEXT("no sequence at '%s'"), *Path));
				return nullptr;
			}
			return Sequence;
		}

		FGuid FindBindingByNameOrGuid(const UMovieScene& MovieScene, const FString& Spec)
		{
			FGuid Parsed;
			if (FGuid::Parse(Spec, Parsed))
			{
				return Parsed;
			}
			for (const FMovieSceneBinding& Binding : MovieScene.GetBindings())
			{
				if (Binding.GetName().Equals(Spec, ESearchCase::IgnoreCase)
					|| MovieScene.GetObjectDisplayName(Binding.GetObjectGuid()).ToString().Equals(Spec, ESearchCase::IgnoreCase))
				{
					return Binding.GetObjectGuid();
				}
			}
			return FGuid();
		}

		UMovieSceneControlRigParameterTrack* FindRigTrack(const UMovieScene& MovieScene, const FString& Spec)
		{
			const FGuid Guid = FindBindingByNameOrGuid(MovieScene, Spec);
			for (const FMovieSceneBinding& Binding : MovieScene.GetBindings())
			{
				for (UMovieSceneTrack* Track : Binding.GetTracks())
				{
					UMovieSceneControlRigParameterTrack* RigTrack = Cast<UMovieSceneControlRigParameterTrack>(Track);
					if (RigTrack == nullptr)
					{
						continue;
					}
					if ((Guid.IsValid() && Binding.GetObjectGuid() == Guid)
						|| RigTrack->GetTrackName().ToString().Equals(Spec, ESearchCase::IgnoreCase))
					{
						return RigTrack;
					}
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> RigTrackJson(const UMovieScene& MovieScene, const FMovieSceneBinding& Binding, UMovieSceneControlRigParameterTrack& Track)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("track"), Track.GetPathName());
			Entry->SetStringField(TEXT("track_name"), Track.GetTrackName().ToString());
			Entry->SetStringField(TEXT("binding"), MovieScene.GetObjectDisplayName(Binding.GetObjectGuid()).ToString());
			Entry->SetStringField(TEXT("binding_guid"), Binding.GetObjectGuid().ToString());
			Entry->SetNumberField(TEXT("sections"), Track.GetAllSections().Num());
			TArray<TSharedPtr<FJsonValue>> Controls;
			if (UControlRig* Rig = Track.GetControlRig())
			{
				Entry->SetStringField(TEXT("rig_class"), Rig->GetClass()->GetPathName());
				if (URigHierarchy* Hierarchy = Rig->GetHierarchy())
				{
					for (const FRigElementKey& Key : Hierarchy->GetAllKeys(false, ERigElementType::Control))
					{
						if (FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key))
						{
							const TSharedRef<FJsonObject> ControlJson = MakeShared<FJsonObject>();
							ControlJson->SetStringField(TEXT("name"), Key.Name.ToString());
							ControlJson->SetStringField(TEXT("type"),
								StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(Control->Settings.ControlType)));
							Controls.Add(MakeShared<FJsonValueObject>(ControlJson));
						}
					}
				}
			}
			Entry->SetArrayField(TEXT("controls"), Controls);
			return Entry;
		}
	}

	void RegisterControlRigRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace ControlRigs;

		Core.RegisterRoute(TEXT("/api/anim/control_rig"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path, SkeletonSpec;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Rigs/CR_Hero")))
					{
						return;
					}
					if (!FPackageName::IsValidLongPackageName(Path))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_path"),
							FString::Printf(TEXT("'%s' is not a package path — use /Game/Folder/Name"), *Path));
						return;
					}
					if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					USkeleton* Skeleton = nullptr;
					USkeletalMesh* Mesh = nullptr;
					if (Body->TryGetStringField(TEXT("skeleton"), SkeletonSpec) && !SkeletonSpec.IsEmpty())
					{
						UObject* Asset = ResolveAsset(SkeletonSpec);
						Mesh = Cast<USkeletalMesh>(Asset);
						Skeleton = Mesh != nullptr ? Mesh->GetSkeleton() : Cast<USkeleton>(Asset);
						if (Skeleton == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("skeleton_not_found"),
								FString::Printf(TEXT("no Skeleton or Skeletal Mesh at '%s'"), *SkeletonSpec));
							return;
						}
					}
					// The New Control Rig factory, then the Rig Hierarchy panel's
					// Import Bones for the skeleton.
					UControlRigBlueprint* Rig = UControlRigBlueprintFactory::CreateNewControlRigAsset(Path, false);
					if (Rig == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("create_failed"),
							TEXT("the Control Rig factory refused the path — see the log"));
						return;
					}
					if (Skeleton != nullptr)
					{
						if (URigHierarchyController* Controller = Rig->GetHierarchyController())
						{
							if (Mesh != nullptr)
							{
								Controller->ImportBones(Mesh, NAME_None, true, true, false, false);
								Rig->SetPreviewMesh(Mesh, true);
							}
							else
							{
								Controller->ImportBones(Skeleton->GetReferenceSkeleton(), NAME_None, true, true, false, false);
							}
						}
					}
					FKismetEditorUtilities::CompileBlueprint(Rig);
					Rig->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = HierarchyJson(*Rig, 500);
					Data->SetStringField(TEXT("message"),
						TEXT("created — add_control puts controls on it, compile after edits, add_track drives it from a sequence"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_track") || Operation == TEXT("list_tracks") || Operation == TEXT("set_control_key"))
				{
					UMovieSceneSequence* Sequence = SequenceOrError(Body, Responder);
					if (Sequence == nullptr)
					{
						return;
					}
					UMovieScene* MovieScene = Sequence->GetMovieScene();

					if (Operation == TEXT("list_tracks"))
					{
						TArray<TSharedPtr<FJsonValue>> Tracks;
						for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
						{
							for (UMovieSceneTrack* Track : Binding.GetTracks())
							{
								if (UMovieSceneControlRigParameterTrack* RigTrack = Cast<UMovieSceneControlRigParameterTrack>(Track))
								{
									Tracks.Add(MakeShared<FJsonValueObject>(RigTrackJson(*MovieScene, Binding, *RigTrack)));
								}
							}
						}
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("sequence"), Sequence->GetPathName());
						Data->SetArrayField(TEXT("tracks"), Tracks);
						Responder->Ok(Data);
						return;
					}

					if (Operation == TEXT("add_track"))
					{
						UControlRigBlueprint* Rig = RigOrError(Body, Responder);
						if (Rig == nullptr)
						{
							return;
						}
						UClass* RigClass = Rig->GetControlRigClass();
						if (RigClass == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("rig_not_compiled"),
								TEXT("the rig has no generated class — compile it first"));
							return;
						}
						UWorld* World = ResolveWorld(Body);
						AActor* Actor = nullptr;
						FString BindingSpec, ActorSpec;
						Body->TryGetStringField(TEXT("binding"), BindingSpec);
						Body->TryGetStringField(TEXT("actor"), ActorSpec);
						FGuid Guid = BindingSpec.IsEmpty() ? FGuid() : FindBindingByNameOrGuid(*MovieScene, BindingSpec);
						if (!ActorSpec.IsEmpty())
						{
							Actor = World != nullptr ? ResolveActor(World, ActorSpec) : nullptr;
							if (Actor == nullptr)
							{
								Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
									FString::Printf(TEXT("no actor matches '%s'"), *ActorSpec));
								return;
							}
							if (!Guid.IsValid())
							{
								Guid = FindBindingByNameOrGuid(*MovieScene, Actor->GetActorLabel());
							}
						}
						else if (Guid.IsValid() && World != nullptr)
						{
							// The bound actor, for the rig's own binding: the
							// binding's name is the actor's label at creation.
							for (TActorIterator<AActor> It(World); It; ++It)
							{
								if (It->GetActorLabel() == MovieScene->GetObjectDisplayName(Guid).ToString())
								{
									Actor = *It;
									break;
								}
							}
						}
						if (!Guid.IsValid() && Actor == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'binding' (a binding name or guid) or 'actor' (a level actor to bind) is required"));
							return;
						}

						const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "AddControlRigTrack", "McpLink Add Control Rig Track"));
						Sequence->Modify();
						MovieScene->Modify();
						if (!Guid.IsValid())
						{
							Guid = Sequence->CreatePossessable(Actor);
						}
						if (MovieScene->FindTrack(UMovieSceneControlRigParameterTrack::StaticClass(), Guid, Rig->GetFName()) != nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
								FString::Printf(TEXT("the binding already has a Control Rig track for '%s'"), *Rig->GetName()));
							return;
						}
						UMovieSceneControlRigParameterTrack* Track = Cast<UMovieSceneControlRigParameterTrack>(
							MovieScene->AddTrack(UMovieSceneControlRigParameterTrack::StaticClass(), Guid));
						if (Track == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("track_failed"),
								TEXT("the movie scene refused a Control Rig track on that binding"));
							return;
						}
						// The track editor's own recipe: a rig instance outered to
						// the track, bound to the actor's skeletal mesh, run once
						// so the section starts from the rig's defaults.
						UControlRig* RigInstance = NewObject<UControlRig>(Track, RigClass, NAME_None, RF_Transactional);
						RigInstance->Modify();
						RigInstance->SetObjectBinding(MakeShared<FControlRigObjectBinding>());
						if (Actor != nullptr)
						{
							if (USkeletalMeshComponent* SkeletalMesh = Actor->FindComponentByClass<USkeletalMeshComponent>())
							{
								RigInstance->GetObjectBinding()->BindToObject(SkeletalMesh);
							}
						}
						RigInstance->Initialize();
						RigInstance->Evaluate_AnyThread();
						Track->Modify();
						FString TrackName;
						Body->TryGetStringField(TEXT("name"), TrackName);
						Track->SetTrackName(FName(*(TrackName.IsEmpty() ? Rig->GetName() : TrackName)));
						UMovieSceneSection* Section = Track->CreateControlRigSection(0, RigInstance, true);
						if (Section != nullptr)
						{
							Section->Modify();
						}
						Sequence->MarkPackageDirty();
						const FMovieSceneBinding* Binding = MovieScene->FindBinding(Guid);
						const TSharedRef<FJsonObject> Data = Binding != nullptr
							? RigTrackJson(*MovieScene, *Binding, *Track) : MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("sequence"), Sequence->GetPathName());
						Data->SetStringField(TEXT("rig"), Rig->GetPathName());
						Data->SetStringField(TEXT("message"), TEXT("track added — set_control_key animates its controls"));
						Responder->Ok(Data);
						return;
					}

					// set_control_key
					FString TrackSpec, ControlName;
					if (!RequireString(Body, TEXT("track"), TrackSpec, Responder, TEXT("a binding name/guid or track name from list_tracks"))
						|| !RequireString(Body, TEXT("control"), ControlName, Responder, TEXT("a control name")))
					{
						return;
					}
					UMovieSceneControlRigParameterTrack* Track = FindRigTrack(*MovieScene, TrackSpec);
					if (Track == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("track_not_found"),
							FString::Printf(TEXT("no Control Rig track matches '%s' — list_tracks shows them"), *TrackSpec));
						return;
					}
					UMovieSceneControlRigParameterSection* Section = Track->GetAllSections().Num() > 0
						? Cast<UMovieSceneControlRigParameterSection>(Track->GetAllSections()[0]) : nullptr;
					UControlRig* Rig = Track->GetControlRig();
					if (Section == nullptr || Rig == nullptr || Rig->GetHierarchy() == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_section"),
							TEXT("the track has no section or rig instance"));
						return;
					}
					FRigControlElement* Control = Rig->GetHierarchy()->Find<FRigControlElement>(
						FRigElementKey(FName(*ControlName), ERigElementType::Control));
					if (Control == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("control_not_found"),
							FString::Printf(TEXT("the rig has no control '%s' — list_tracks shows them"), *ControlName));
						return;
					}
					FRigControlValue Value;
					FString Error;
					if (!MakeControlValue(Control->Settings.ControlType, Body->TryGetField(TEXT("value")), Value, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"), Error);
						return;
					}
					FFrameNumber Frame;
					double Seconds = 0.0;
					int32 DisplayFrame = 0;
					if (Body->TryGetNumberField(TEXT("time"), Seconds))
					{
						Frame = MovieScene->GetTickResolution().AsFrameTime(Seconds).RoundToFrame();
					}
					else if (Body->TryGetNumberField(TEXT("frame"), DisplayFrame))
					{
						Frame = FFrameRate::TransformTime(FFrameTime(DisplayFrame), MovieScene->GetDisplayRate(),
							MovieScene->GetTickResolution()).RoundToFrame();
					}
					else
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'time' (seconds) or 'frame' (display rate) is required"));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "KeyControl", "McpLink Key Control Rig Control"));
					Section->Modify();
					const FName Name(*ControlName);
					switch (Control->Settings.ControlType)
					{
					case ERigControlType::Bool:
						Section->AddBoolParameterKey(Name, Frame, Value.Get<bool>());
						break;
					case ERigControlType::Float:
					case ERigControlType::ScaleFloat:
						Section->AddScalarParameterKey(Name, Frame, Value.Get<float>());
						break;
					case ERigControlType::Integer:
						Section->AddIntegerParameterKey(Name, Frame, Value.Get<int32>());
						break;
					case ERigControlType::Vector2D:
					{
						const FVector3f V = Value.Get<FVector3f>();
						Section->AddVector2DParameterKey(Name, Frame, FVector2D(V.X, V.Y));
						break;
					}
					case ERigControlType::Position:
					case ERigControlType::Scale:
					case ERigControlType::Rotator:
						Section->AddVectorParameterKey(Name, Frame, FVector(Value.Get<FVector3f>()));
						break;
					default:
						Section->AddTransformParameterKey(Name, Frame,
							Value.GetAsTransform(Control->Settings.ControlType, Control->Settings.PrimaryAxis));
						break;
					}
					Section->ExpandToFrame(Frame);
					Sequence->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("sequence"), Sequence->GetPathName());
					Data->SetStringField(TEXT("track"), Track->GetPathName());
					Data->SetStringField(TEXT("control"), ControlName);
					Data->SetNumberField(TEXT("tick"), Frame.Value);
					Data->SetNumberField(TEXT("frame"), FFrameRate::TransformTime(FFrameTime(Frame),
						MovieScene->GetTickResolution(), MovieScene->GetDisplayRate()).RoundToFrame().Value);
					Data->SetField(TEXT("value"), ControlValueJson(*Control, Value));
					Responder->Ok(Data);
					return;
				}

				UControlRigBlueprint* Rig = RigOrError(Body, Responder);
				if (Rig == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					Responder->Ok(HierarchyJson(*Rig, FMath::Clamp(IntOr(Body, TEXT("max_elements"), 500), 1, 10000)));
					return;
				}

				if (Operation == TEXT("compile"))
				{
					FKismetEditorUtilities::CompileBlueprint(Rig);
					const TSharedRef<FJsonObject> Data = HierarchyJson(*Rig, 0);
					Data->SetStringField(TEXT("status"), Rig->Status == BS_Error ? TEXT("error")
						: (Rig->Status == BS_UpToDate ? TEXT("up_to_date") : TEXT("dirty")));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Rig, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("rig"), Rig->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				URigHierarchy* Hierarchy = Rig->GetHierarchy();
				URigHierarchyController* Controller = Rig->GetHierarchyController();
				if (Hierarchy == nullptr || Controller == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_hierarchy"),
						TEXT("the rig has no hierarchy controller"));
					return;
				}

				if (Operation == TEXT("add_bone") || Operation == TEXT("add_null") || Operation == TEXT("add_control"))
				{
					FString NameString;
					if (!RequireString(Body, TEXT("name"), NameString, Responder, TEXT("the element's name")))
					{
						return;
					}
					const FName Name(*NameString);
					bool bFailed = false;
					const FRigElementKey Parent = ParentKeyOrError(*Hierarchy, Body, Responder, bFailed);
					if (bFailed)
					{
						return;
					}
					const ERigElementType NewType = Operation == TEXT("add_bone") ? ERigElementType::Bone
						: (Operation == TEXT("add_null") ? ERigElementType::Null : ERigElementType::Control);
					if (Hierarchy->GetIndex(FRigElementKey(Name, NewType)) != INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("the rig already has a %s named '%s'"),
								*StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(NewType)), *NameString));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "AddRigElement", "McpLink Add Rig Element"));
					Rig->UObject::Modify();
					FRigElementKey Key;
					const TSharedPtr<FJsonObject>* TransformObject = nullptr;
					FTransform Transform = FTransform::Identity;
					if (Body->TryGetObjectField(TEXT("transform"), TransformObject))
					{
						ReadTransform(*TransformObject, Transform);
					}
					if (NewType == ERigElementType::Bone)
					{
						Key = Controller->AddBone(Name, Parent, Transform, true, ERigBoneType::User, true, false);
					}
					else if (NewType == ERigElementType::Null)
					{
						Key = Controller->AddNull(Name, Parent, Transform, true, true, false);
					}
					else
					{
						FString TypeName;
						if (!RequireString(Body, TEXT("type"), TypeName, Responder,
								TEXT("Bool, Float, Integer, Vector2D, Position, Scale, Rotator, Transform, TransformNoScale, EulerTransform or ScaleFloat")))
						{
							return;
						}
						const int64 TypeValue = StaticEnum<ERigControlType>()->GetValueByNameString(TypeName);
						if (TypeValue == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_type"),
								FString::Printf(TEXT("'%s' is not a control type"), *TypeName));
							return;
						}
						FRigControlSettings Settings;
						Settings.ControlType = static_cast<ERigControlType>(TypeValue);
						Settings.SetupLimitArrayForType(false, false, false);
						FString DisplayName, Shape;
						Settings.DisplayName = Body->TryGetStringField(TEXT("display_name"), DisplayName) && !DisplayName.IsEmpty()
							? FName(*DisplayName) : Name;
						if (Body->TryGetStringField(TEXT("shape"), Shape) && !Shape.IsEmpty())
						{
							Settings.ShapeName = FName(*Shape);
						}
						Settings.bShapeVisible = BoolOr(Body, TEXT("shape_visible"), true);
						FRigControlValue Value;
						FString Error;
						if (Body->HasField(TEXT("value")))
						{
							if (!MakeControlValue(Settings.ControlType, Body->TryGetField(TEXT("value")), Value, Error))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"), Error);
								return;
							}
						}
						else
						{
							// The type's zero — identity for transforms.
							MakeControlValue(Settings.ControlType,
								Settings.ControlType == ERigControlType::Bool ? MakeShared<FJsonValueBoolean>(false)
								: (Settings.ControlType == ERigControlType::Vector2D
									? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0)}))
									: (Settings.ControlType == ERigControlType::Position || Settings.ControlType == ERigControlType::Rotator
										? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueArray>(VectorToJson(FVector::ZeroVector)))
										: (Settings.ControlType == ERigControlType::Scale
											? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueArray>(VectorToJson(FVector::OneVector)))
											: (Settings.ControlType == ERigControlType::Float || Settings.ControlType == ERigControlType::Integer || Settings.ControlType == ERigControlType::ScaleFloat
												? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNumber>(Settings.ControlType == ERigControlType::ScaleFloat ? 1.0 : 0.0))
												: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())))))),
								Value, Error);
						}
						const TSharedPtr<FJsonObject>* OffsetObject = nullptr;
						FTransform Offset = FTransform::Identity;
						if (Body->TryGetObjectField(TEXT("offset"), OffsetObject))
						{
							ReadTransform(*OffsetObject, Offset);
						}
						Key = Controller->AddControl(Name, Parent, Settings, Value, Offset, FTransform::Identity, true, false);
					}
					if (!Key.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_refused"),
							TEXT("the hierarchy controller refused the element — see the log"));
						return;
					}
					Rig->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = HierarchyJson(*Rig, 500);
					Data->SetStringField(TEXT("added"), Key.Name.ToString());
					Data->SetStringField(TEXT("added_type"), StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Key.Type)));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_element"))
				{
					FString NameString, TypeName;
					if (!RequireString(Body, TEXT("name"), NameString, Responder, TEXT("the element's name")))
					{
						return;
					}
					Body->TryGetStringField(TEXT("type"), TypeName);
					FRigElementKey Key;
					if (!TypeName.IsEmpty())
					{
						const int64 TypeValue = StaticEnum<ERigElementType>()->GetValueByNameString(TypeName);
						if (TypeValue == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_type"),
								FString::Printf(TEXT("'%s' is not an element type — Bone, Null or Control"), *TypeName));
							return;
						}
						Key = FRigElementKey(FName(*NameString), static_cast<ERigElementType>(TypeValue));
					}
					else
					{
						for (const ERigElementType Type : {ERigElementType::Control, ERigElementType::Null, ERigElementType::Bone})
						{
							if (Hierarchy->GetIndex(FRigElementKey(FName(*NameString), Type)) != INDEX_NONE)
							{
								Key = FRigElementKey(FName(*NameString), Type);
								break;
							}
						}
					}
					if (!Key.IsValid() || Hierarchy->GetIndex(Key) == INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("element_not_found"),
							FString::Printf(TEXT("the rig has no element '%s'"), *NameString));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "RemoveRigElement", "McpLink Remove Rig Element"));
					Rig->UObject::Modify();
					if (!Controller->RemoveElement(Key, true, false))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("remove_refused"),
							TEXT("the hierarchy controller refused to remove the element"));
						return;
					}
					Rig->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = HierarchyJson(*Rig, 500);
					Data->SetStringField(TEXT("removed"), NameString);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_control_value"))
				{
					FString ControlName;
					if (!RequireString(Body, TEXT("control"), ControlName, Responder, TEXT("a control name")))
					{
						return;
					}
					FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(FRigElementKey(FName(*ControlName), ERigElementType::Control));
					if (Control == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("control_not_found"),
							FString::Printf(TEXT("the rig has no control '%s'"), *ControlName));
						return;
					}
					FRigControlValue Value;
					FString Error;
					if (!MakeControlValue(Control->Settings.ControlType, Body->TryGetField(TEXT("value")), Value, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"), Error);
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "SetControlValue", "McpLink Set Control Value"));
					Rig->UObject::Modify();
					const ERigControlValueType ValueType = BoolOr(Body, TEXT("initial"), false)
						? ERigControlValueType::Initial : ERigControlValueType::Current;
					Hierarchy->SetControlValue(Control, Value, ValueType, true, false, false);
					Rig->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("rig"), Rig->GetPathName());
					Data->SetStringField(TEXT("control"), ControlName);
					Data->SetStringField(TEXT("value_type"), ValueType == ERigControlValueType::Initial ? TEXT("initial") : TEXT("current"));
					Data->SetField(TEXT("value"), ControlValueJson(*Control, Hierarchy->GetControlValue(Control, ValueType)));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected create, info, add_bone, add_null, add_control, ")
						TEXT("remove_element, set_control_value, compile, save, add_track, list_tracks or set_control_key"), *Operation));
			});
	}
}
