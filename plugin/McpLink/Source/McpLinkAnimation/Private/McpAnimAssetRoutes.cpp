// Animation assets: Montages, Blend Spaces, Aim Offsets and Composites.
//
// These are all UAnimationAsset subclasses whose structure lives in ordinary
// public properties — a montage is slot tracks of FAnimSegments plus a list of
// FCompositeSections, a blend space is samples on one or two parameter axes.
// The editors for them are thin wrappers over exactly that data, so authoring
// here writes the same fields and the assets open as if hand-built.

#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "AnimationModifier.h"
#include "AnimationModifiersAssetUserData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/AnimMontageFactory.h"
#include "JsonObjectConverter.h"
#include "McpAnimUtils.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace McpLink
{
	namespace AnimAssets
	{
		/// Every animation asset kind this route can create, and the class it
		/// maps to. Blend spaces and aim offsets differ only by class.
		struct FAssetKind
		{
			const TCHAR* Name;
			UClass* (*Class)();
		};

		const FAssetKind Kinds[] = {
			{TEXT("montage"), []() { return UAnimMontage::StaticClass(); }},
			{TEXT("composite"), []() { return UAnimComposite::StaticClass(); }},
			{TEXT("blend_space"), []() { return UBlendSpace::StaticClass(); }},
			{TEXT("blend_space_1d"), []() { return static_cast<UClass*>(UBlendSpace1D::StaticClass()); }},
			{TEXT("aim_offset"), []() { return static_cast<UClass*>(UAimOffsetBlendSpace::StaticClass()); }},
			{TEXT("aim_offset_1d"), []() { return static_cast<UClass*>(UAimOffsetBlendSpace1D::StaticClass()); }},
		};

		FString KindNames()
		{
			TArray<FString> Names;
			for (const FAssetKind& Kind : Kinds)
			{
				Names.Add(Kind.Name);
			}
			return FString::Join(Names, TEXT(", "));
		}

		UClass* ClassForKind(const FString& Name)
		{
			for (const FAssetKind& Kind : Kinds)
			{
				if (Name.Equals(Kind.Name, ESearchCase::IgnoreCase))
				{
					return Kind.Class();
				}
			}
			return nullptr;
		}

		/// Every loaded Animation Modifier class — the engine's library
		/// (distance curves, motion extraction, footsteps, root re-orient …)
		/// and any Blueprint modifier that has been loaded.
		TArray<UClass*> ModifierClasses()
		{
			TArray<UClass*> Out;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (!It->IsChildOf(UAnimationModifier::StaticClass()) || *It == UAnimationModifier::StaticClass()
					|| It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
					|| It->GetName().StartsWith(TEXT("SKEL_")) || It->GetName().StartsWith(TEXT("REINST_")))
				{
					continue;
				}
				Out.Add(*It);
			}
			Out.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
			return Out;
		}

		TSharedRef<FJsonObject> ModifierJson(const UAnimationModifier& Modifier, UAnimSequence* Sequence)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("modifier"), Modifier.GetPathName());
			Entry->SetStringField(TEXT("class"), Modifier.GetClass()->GetName());
			if (Sequence != nullptr)
			{
				Entry->SetBoolField(TEXT("applied"), Modifier.IsLatestRevisionApplied(Sequence));
				Entry->SetBoolField(TEXT("can_revert"), Modifier.CanRevert(Sequence));
			}
			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(Modifier.GetClass(), &Modifier, Properties, CPF_Edit, 0,
				nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
			Entry->SetObjectField(TEXT("properties"), Properties);
			return Entry;
		}

		TArray<TSharedPtr<FJsonValue>> AppliedModifiersJson(UAnimSequence& Sequence)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			if (const UAnimationModifiersAssetUserData* UserData = Sequence.GetAssetUserData<UAnimationModifiersAssetUserData>())
			{
				for (const UAnimationModifier* Modifier : UserData->GetAnimationModifierInstances())
				{
					if (Modifier != nullptr)
					{
						Out.Add(MakeShared<FJsonValueObject>(ModifierJson(*Modifier, &Sequence)));
					}
				}
			}
			return Out;
		}

		UAnimationAsset* AnimAssetOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("asset"), Path, Responder,
					TEXT("an animation asset path, e.g. /Game/Anims/AM_Attack")))
			{
				return nullptr;
			}
			UAnimationAsset* Asset = Cast<UAnimationAsset>(ResolveAsset(Path));
			if (Asset == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
					FString::Printf(TEXT("no animation asset at '%s'"), *Path));
			}
			return Asset;
		}

		TSharedRef<FJsonObject> SegmentToJson(const FAnimSegment& Segment, int32 Index)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("index"), Index);
			Object->SetStringField(TEXT("animation"),
				Segment.GetAnimReference() != nullptr ? Segment.GetAnimReference()->GetPathName() : FString());
			Object->SetNumberField(TEXT("start_position"), Segment.StartPos);
			Object->SetNumberField(TEXT("anim_start"), Segment.AnimStartTime);
			Object->SetNumberField(TEXT("anim_end"), Segment.AnimEndTime);
			Object->SetNumberField(TEXT("play_rate"), Segment.AnimPlayRate);
			Object->SetNumberField(TEXT("loop_count"), Segment.LoopingCount);
			return Object;
		}

		void AppendTrack(const TSharedRef<FJsonObject>& Into, const FAnimTrack& Track)
		{
			TArray<TSharedPtr<FJsonValue>> Segments;
			for (int32 Index = 0; Index < Track.AnimSegments.Num(); ++Index)
			{
				Segments.Add(MakeShared<FJsonValueObject>(SegmentToJson(Track.AnimSegments[Index], Index)));
			}
			Into->SetArrayField(TEXT("segments"), Segments);
		}

		/// Fill a segment from the request. `animation` is required.
		bool ReadSegment(
			const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder,
			FAnimSegment& Out)
		{
			FString AnimPath;
			if (!RequireString(Body, TEXT("animation"), AnimPath, Responder,
					TEXT("an Anim Sequence path to place in the track")))
			{
				return false;
			}
			UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(ResolveAsset(AnimPath));
			if (Sequence == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
					FString::Printf(TEXT("no animation sequence at '%s'"), *AnimPath));
				return false;
			}
			Out.SetAnimReference(Sequence, /*bInitialize*/ true);
			Out.StartPos = DoubleOr(Body, TEXT("start_position"), Out.StartPos);
			Out.AnimStartTime = DoubleOr(Body, TEXT("anim_start"), Out.AnimStartTime);
			Out.AnimEndTime = DoubleOr(Body, TEXT("anim_end"), Out.AnimEndTime);
			Out.AnimPlayRate = DoubleOr(Body, TEXT("play_rate"), Out.AnimPlayRate);
			Out.LoopingCount = IntOr(Body, TEXT("loop_count"), Out.LoopingCount);
			return true;
		}

		TSharedRef<FJsonObject> BlendSampleToJson(const FBlendSample& Sample, int32 Index)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("index"), Index);
			Object->SetStringField(TEXT("animation"),
				Sample.Animation != nullptr ? Sample.Animation->GetPathName() : FString());
			Object->SetArrayField(TEXT("position"),
				{MakeShared<FJsonValueNumber>(Sample.SampleValue.X),
					MakeShared<FJsonValueNumber>(Sample.SampleValue.Y),
					MakeShared<FJsonValueNumber>(Sample.SampleValue.Z)});
			Object->SetNumberField(TEXT("rate_scale"), Sample.RateScale);
			return Object;
		}
	}

	using namespace AnimAssets;
	using namespace McpLink::Anim;

	void RegisterAnimAssetRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/anim/assets"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Kind, Path;
					if (!RequireString(Body, TEXT("kind"), Kind, Responder, *KindNames())
						|| !RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Anims/AM_Attack")))
					{
						return;
					}
					UClass* Class = ClassForKind(Kind);
					if (Class == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_kind"),
							FString::Printf(TEXT("'kind' must be one of %s"), *KindNames()));
						return;
					}

					// The skeleton can be named outright, or inferred from the
					// animation the asset is being built around.
					USkeleton* Skeleton = nullptr;
					FString SkeletonPath, AnimPath;
					UAnimSequence* Source = nullptr;
					if (Body->TryGetStringField(TEXT("animation"), AnimPath) && !AnimPath.IsEmpty())
					{
						Source = Cast<UAnimSequence>(ResolveAsset(AnimPath));
						if (Source == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
								FString::Printf(TEXT("no Anim Sequence at '%s'"), *AnimPath));
							return;
						}
						Skeleton = Source->GetSkeleton();
					}
					if (Body->TryGetStringField(TEXT("skeleton"), SkeletonPath) && !SkeletonPath.IsEmpty())
					{
						Skeleton = Cast<USkeleton>(ResolveAsset(SkeletonPath));
						if (Skeleton == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("skeleton_not_found"),
								FString::Printf(TEXT("no Skeleton at '%s'"), *SkeletonPath));
							return;
						}
					}
					if (Skeleton == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'skeleton' is required (or 'animation', whose skeleton is then used)"));
						return;
					}
					if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateAnimAsset", "McpLink Create Animation Asset"));
					UPackage* Package = CreatePackage(*Path);
					UAnimationAsset* Asset = NewObject<UAnimationAsset>(
						Package, Class, FName(*FPackageName::GetShortName(Path)),
						RF_Public | RF_Standalone | RF_Transactional);
					Asset->SetSkeleton(Skeleton);

					if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
					{
						FString SlotName = TEXT("DefaultSlot");
						Body->TryGetStringField(TEXT("slot"), SlotName);
						FSlotAnimationTrack& Slot = FindOrAddSlot(Montage, FName(*SlotName));
						if (Source != nullptr)
						{
							FAnimSegment Segment;
							Segment.SetAnimReference(Source, /*bInitialize*/ true);
							Slot.AnimTrack.AnimSegments.Add(Segment);
						}
						// A montage with no section at t=0 is invalid; this is
						// the same guard the montage factory applies.
						UAnimMontageFactory::EnsureStartingSection(Montage);
						RefreshMontage(Montage);
					}
					else if (UAnimComposite* Composite = Cast<UAnimComposite>(Asset))
					{
						if (Source != nullptr)
						{
							FAnimSegment Segment;
							Segment.SetAnimReference(Source, /*bInitialize*/ true);
							Composite->AnimationTrack.AnimSegments.Add(Segment);
							Composite->SetCompositeLength(Composite->AnimationTrack.GetLength());
						}
					}

					FAssetRegistryModule::AssetCreated(Asset);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), Asset->GetPathName());
					Data->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
					Data->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
					Data->SetStringField(TEXT("message"),
						TEXT("created in memory — author it, then save"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_kinds"))
				{
					TArray<TSharedPtr<FJsonValue>> Names;
					for (const FAssetKind& Kind : Kinds)
					{
						Names.Add(MakeShared<FJsonValueString>(Kind.Name));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("kinds"), Names);
					Responder->Ok(Data);
					return;
				}

				// ---- everything below targets an existing asset ------------
				if (Operation == TEXT("list_modifiers"))
				{
					TArray<TSharedPtr<FJsonValue>> Classes;
					for (UClass* Class : ModifierClasses())
					{
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("class"), Class->GetName());
						Entry->SetStringField(TEXT("path"), Class->GetPathName());
						Entry->SetBoolField(TEXT("blueprint"), Class->ClassGeneratedBy != nullptr);
						const TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>();
						FJsonObjectConverter::UStructToJsonObject(Class, Class->GetDefaultObject(), Defaults, CPF_Edit, 0,
							nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
						Entry->SetObjectField(TEXT("defaults"), Defaults);
						Classes.Add(MakeShared<FJsonValueObject>(Entry));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("modifiers"), Classes);
					Data->SetStringField(TEXT("note"),
						TEXT("Blueprint modifiers appear once loaded — asset_ops load / search_assets class AnimationModifier finds them"));
					Responder->Ok(Data);
					return;
				}

				static const TCHAR* AssetOperations[] = {
					TEXT("info"), TEXT("save"), TEXT("add_slot"), TEXT("add_section"),
					TEXT("set_section"), TEXT("remove_section"), TEXT("add_segment"),
					TEXT("remove_segment"), TEXT("add_sample"), TEXT("set_sample"),
					TEXT("remove_sample"), TEXT("set_axis"), TEXT("list_applied_modifiers"),
					TEXT("apply_modifier"), TEXT("revert_modifier"), TEXT("remove_modifier")};
				bool bKnown = false;
				for (const TCHAR* Known : AssetOperations)
				{
					bKnown = bKnown || Operation == Known;
				}
				if (!bKnown)
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
						FString::Printf(
							TEXT("unknown operation '%s' — use list_kinds, create, info, add_slot, ")
							TEXT("add_section, set_section, remove_section, add_segment, remove_segment, ")
							TEXT("add_sample, set_sample, remove_sample, set_axis, list_modifiers, ")
							TEXT("list_applied_modifiers, apply_modifier, revert_modifier, remove_modifier, or save"),
							*Operation));
					return;
				}
				UAnimationAsset* Asset = AnimAssetOrError(Body, Responder);
				if (Asset == nullptr)
				{
					return;
				}

				if (Operation == TEXT("list_applied_modifiers") || Operation == TEXT("apply_modifier")
					|| Operation == TEXT("revert_modifier") || Operation == TEXT("remove_modifier"))
				{
					UAnimSequence* Sequence = Cast<UAnimSequence>(Asset);
					if (Sequence == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("not_a_sequence"),
							TEXT("Animation Modifiers run on Animation Sequences"));
						return;
					}
					if (Operation == TEXT("list_applied_modifiers"))
					{
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("asset"), Sequence->GetPathName());
						Data->SetArrayField(TEXT("modifiers"), AppliedModifiersJson(*Sequence));
						Responder->Ok(Data);
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "AnimModifier", "McpLink Animation Modifier"));
					Sequence->Modify();
					if (Operation == TEXT("apply_modifier"))
					{
						FString ClassSpec;
						if (!RequireString(Body, TEXT("modifier"), ClassSpec, Responder,
								TEXT("a modifier class from list_modifiers, e.g. DistanceCurveModifier")))
						{
							return;
						}
						UClass* Class = nullptr;
						for (UClass* Candidate : ModifierClasses())
						{
							if (Candidate->GetName() == ClassSpec || Candidate->GetPathName() == ClassSpec
								|| Candidate->GetName().Equals(ClassSpec, ESearchCase::IgnoreCase))
							{
								Class = Candidate;
								break;
							}
						}
						if (Class == nullptr)
						{
							Class = Cast<UClass>(ResolveClass(ClassSpec));
						}
						if (Class == nullptr || !Class->IsChildOf(UAnimationModifier::StaticClass()))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("modifier_not_found"),
								FString::Printf(TEXT("'%s' is not an Animation Modifier class — list_modifiers shows them"), *ClassSpec));
							return;
						}
						// The Animation Modifiers panel's "Add": an instance on the
						// sequence's asset user data, which is what re-applies it
						// after edits and remembers it for revert.
						if (!UAnimationModifiersAssetUserData::AddAnimationModifierOfClass(Sequence, Class))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_refused"),
								TEXT("the sequence refused the modifier instance"));
							return;
						}
						UAnimationModifiersAssetUserData* UserData = Sequence->GetAssetUserData<UAnimationModifiersAssetUserData>();
						UAnimationModifier* Instance = UserData != nullptr && UserData->GetAnimationModifierInstances().Num() > 0
							? UserData->GetAnimationModifierInstances().Last() : nullptr;
						if (Instance == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_instance"),
								TEXT("the modifier instance was not created"));
							return;
						}
						const TSharedPtr<FJsonObject>* Properties = nullptr;
						if (Body->TryGetObjectField(TEXT("properties"), Properties)
							&& !FJsonObjectConverter::JsonObjectToUStruct(Properties->ToSharedRef(), Instance->GetClass(), Instance, 0, 0))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_properties"),
								TEXT("'properties' did not apply — keys are the modifier's UPROPERTY names (list_modifiers shows defaults)"));
							return;
						}
						Instance->ApplyToAnimationSequence(Sequence);
						Sequence->MarkPackageDirty();
						const TSharedRef<FJsonObject> Data = ModifierJson(*Instance, Sequence);
						Data->SetStringField(TEXT("asset"), Sequence->GetPathName());
						Data->SetArrayField(TEXT("modifiers"), AppliedModifiersJson(*Sequence));
						Responder->Ok(Data);
						return;
					}
					// revert / remove: by class name or index into list_applied_modifiers.
					UAnimationModifiersAssetUserData* UserData = Sequence->GetAssetUserData<UAnimationModifiersAssetUserData>();
					FString Spec;
					Body->TryGetStringField(TEXT("modifier"), Spec);
					UAnimationModifier* Instance = nullptr;
					if (UserData != nullptr)
					{
						const TArray<UAnimationModifier*>& Instances = UserData->GetAnimationModifierInstances();
						const int32 Index = Spec.IsNumeric() ? FCString::Atoi(*Spec) : IntOr(Body, TEXT("index"), Spec.IsEmpty() && Instances.Num() == 1 ? 0 : -1);
						if (Instances.IsValidIndex(Index))
						{
							Instance = Instances[Index];
						}
						else
						{
							for (UAnimationModifier* Candidate : Instances)
							{
								if (Candidate != nullptr && (Candidate->GetClass()->GetName() == Spec || Candidate->GetName() == Spec
									|| Candidate->GetClass()->GetName().Equals(Spec, ESearchCase::IgnoreCase)))
								{
									Instance = Candidate;
									break;
								}
							}
						}
					}
					if (Instance == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("modifier_not_found"),
							FString::Printf(TEXT("no applied modifier matches '%s' — list_applied_modifiers shows them"), *Spec));
						return;
					}
					Instance->RevertFromAnimationSequence(Sequence);
					if (Operation == TEXT("remove_modifier"))
					{
						// The panel's remove goes through a protected member; the
						// instance list is a UPROPERTY, so reflection drops it.
						UserData->Modify();
						if (FArrayProperty* Property = FindFProperty<FArrayProperty>(UserData->GetClass(), TEXT("AnimationModifierInstances")))
						{
							FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(UserData));
							FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(Property->Inner);
							for (int32 Index = Helper.Num() - 1; Index >= 0; --Index)
							{
								if (Inner != nullptr && Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)) == Instance)
								{
									Helper.RemoveValues(Index, 1);
								}
							}
						}
					}
					Sequence->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), Sequence->GetPathName());
					Data->SetStringField(TEXT("applied"), Operation);
					Data->SetArrayField(TEXT("modifiers"), AppliedModifiersJson(*Sequence));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), Asset->GetPathName());
					Data->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
					Data->SetStringField(TEXT("skeleton"),
						Asset->GetSkeleton() != nullptr ? Asset->GetSkeleton()->GetPathName() : FString());

					if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
					{
						Data->SetNumberField(TEXT("length"), Montage->GetPlayLength());
						TArray<TSharedPtr<FJsonValue>> Slots;
						for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
						{
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("slot"), Slot.SlotName.ToString());
							AppendTrack(Item, Slot.AnimTrack);
							Slots.Add(MakeShared<FJsonValueObject>(Item));
						}
						Data->SetArrayField(TEXT("slots"), Slots);

						TArray<TSharedPtr<FJsonValue>> Sections;
						for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
						{
							const FCompositeSection& Section = Montage->CompositeSections[Index];
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetNumberField(TEXT("index"), Index);
							Item->SetStringField(TEXT("name"), Section.SectionName.ToString());
							Item->SetNumberField(TEXT("start_time"), Section.GetTime());
							Item->SetNumberField(TEXT("length"), Montage->GetSectionLength(Index));
							Item->SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
							Sections.Add(MakeShared<FJsonValueObject>(Item));
						}
						Data->SetArrayField(TEXT("sections"), Sections);
					}
					else if (UAnimComposite* Composite = Cast<UAnimComposite>(Asset))
					{
						Data->SetNumberField(TEXT("length"), Composite->GetPlayLength());
						AppendTrack(Data, Composite->AnimationTrack);
					}
					else if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset))
					{
						TArray<TSharedPtr<FJsonValue>> Axes;
						const int32 AxisCount = BlendSpace->IsA<UBlendSpace1D>() ? 1 : 2;
						for (int32 Index = 0; Index < AxisCount; ++Index)
						{
							const FBlendParameter& Parameter = BlendSpace->GetBlendParameter(Index);
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetNumberField(TEXT("axis"), Index);
							Item->SetStringField(TEXT("name"), Parameter.DisplayName);
							Item->SetNumberField(TEXT("min"), Parameter.Min);
							Item->SetNumberField(TEXT("max"), Parameter.Max);
							Item->SetNumberField(TEXT("grid_divisions"), Parameter.GridNum);
							Axes.Add(MakeShared<FJsonValueObject>(Item));
						}
						Data->SetArrayField(TEXT("axes"), Axes);

						TArray<TSharedPtr<FJsonValue>> Samples;
						const TArray<FBlendSample>& SampleData = BlendSpace->GetBlendSamples();
						for (int32 Index = 0; Index < SampleData.Num(); ++Index)
						{
							Samples.Add(MakeShared<FJsonValueObject>(
								BlendSampleToJson(SampleData[Index], Index)));
						}
						Data->SetArrayField(TEXT("samples"), Samples);
					}
					else if (UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(Asset))
					{
						Data->SetNumberField(TEXT("length"), Sequence->GetPlayLength());
					}
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

				// ------------------------------------------- montage edits ---
				if (Operation == TEXT("add_slot") || Operation == TEXT("add_section")
					|| Operation == TEXT("set_section") || Operation == TEXT("remove_section"))
				{
					UAnimMontage* Montage = Cast<UAnimMontage>(Asset);
					if (Montage == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrong_asset_type"),
							FString::Printf(TEXT("'%s' is a %s — slots and sections are montage-only"),
								*Asset->GetPathName(), *Asset->GetClass()->GetName()));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditMontage", "McpLink Edit Montage"));
					Montage->Modify();

					if (Operation == TEXT("add_slot"))
					{
						FString SlotName;
						if (!RequireString(Body, TEXT("slot"), SlotName, Responder,
								TEXT("the slot name, e.g. UpperBody")))
						{
							return;
						}
						if (FindSlot(Montage, FName(*SlotName)) != nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("slot_exists"),
								FString::Printf(TEXT("montage already has a slot '%s'"), *SlotName));
							return;
						}
						Montage->AddSlot(FName(*SlotName));
						RefreshMontage(Montage);
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("slot"), SlotName);
						Data->SetNumberField(TEXT("slot_count"), Montage->SlotAnimTracks.Num());
						Responder->Ok(Data);
						return;
					}

					FString SectionName;
					if (!RequireString(Body, TEXT("name"), SectionName, Responder,
							TEXT("the section name")))
					{
						return;
					}
					const int32 Existing = Montage->GetSectionIndex(FName(*SectionName));

					if (Operation == TEXT("add_section"))
					{
						if (Existing != INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("section_exists"),
								FString::Printf(TEXT("montage already has a section '%s'"), *SectionName));
							return;
						}
						FCompositeSection Section;
						Section.SectionName = FName(*SectionName);
						Section.Link(Montage, DoubleOr(Body, TEXT("time"), 0.0));
						FString NextSection;
						if (Body->TryGetStringField(TEXT("next_section"), NextSection))
						{
							Section.NextSectionName = FName(*NextSection);
						}
						Montage->CompositeSections.Add(Section);
					}
					else if (Operation == TEXT("set_section"))
					{
						if (Existing == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("section_not_found"),
								FString::Printf(TEXT("montage has no section '%s'"), *SectionName));
							return;
						}
						FCompositeSection& Section = Montage->CompositeSections[Existing];
						double Time = 0.0;
						if (Body->TryGetNumberField(TEXT("time"), Time))
						{
							Section.SetTime(Time);
						}
						FString NextSection;
						if (Body->TryGetStringField(TEXT("next_section"), NextSection))
						{
							Section.NextSectionName = FName(*NextSection);
						}
					}
					else
					{
						if (Existing == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("section_not_found"),
								FString::Printf(TEXT("montage has no section '%s'"), *SectionName));
							return;
						}
						Montage->CompositeSections.RemoveAt(Existing);
						UAnimMontageFactory::EnsureStartingSection(Montage);
					}

					// The montage editor keeps sections in start-time order and
					// GetSectionIndexFromPosition assumes it.
					Montage->CompositeSections.Sort(
						[](const FCompositeSection& A, const FCompositeSection& B)
						{ return A.GetTime() < B.GetTime(); });
					RefreshMontage(Montage);
					TArray<TSharedPtr<FJsonValue>> Sections;
					for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
					{
						const FCompositeSection& Section = Montage->CompositeSections[Index];
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Section.SectionName.ToString());
						Item->SetNumberField(TEXT("start_time"), Section.GetTime());
						Item->SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
						Sections.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("sections"), Sections);
					Responder->Ok(Data);
					return;
				}

				// --------------------------------------------- track edits ---
				if (Operation == TEXT("add_segment") || Operation == TEXT("remove_segment"))
				{
					FAnimTrack* Track = nullptr;
					UAnimMontage* Montage = Cast<UAnimMontage>(Asset);
					if (Montage != nullptr)
					{
						FString SlotName = Montage->SlotAnimTracks.IsEmpty()
							? TEXT("DefaultSlot")
							: Montage->SlotAnimTracks[0].SlotName.ToString();
						Body->TryGetStringField(TEXT("slot"), SlotName);
						if (FSlotAnimationTrack* Slot = FindSlot(Montage, FName(*SlotName)))
						{
							Track = &Slot->AnimTrack;
						}
						if (Track == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("slot_not_found"),
								FString::Printf(TEXT("montage has no slot '%s' — add_slot first"), *SlotName));
							return;
						}
					}
					else if (UAnimComposite* Composite = Cast<UAnimComposite>(Asset))
					{
						Track = &Composite->AnimationTrack;
					}
					else
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrong_asset_type"),
							FString::Printf(TEXT("'%s' is a %s — segments live on montages and composites"),
								*Asset->GetPathName(), *Asset->GetClass()->GetName()));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditAnimTrack", "McpLink Edit Animation Track"));
					Asset->Modify();
					if (Operation == TEXT("add_segment"))
					{
						FAnimSegment Segment;
						if (!ReadSegment(Body, Responder, Segment))
						{
							return;
						}
						Track->AnimSegments.Add(Segment);
					}
					else
					{
						const int32 Index = IntOr(Body, TEXT("index"), -1);
						if (!Track->AnimSegments.IsValidIndex(Index))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_index"),
								FString::Printf(TEXT("'index' must be 0..%d"), Track->AnimSegments.Num() - 1));
							return;
						}
						Track->AnimSegments.RemoveAt(Index);
					}

					if (Montage != nullptr)
					{
						RefreshMontage(Montage);
					}
					else if (UAnimComposite* Composite = Cast<UAnimComposite>(Asset))
					{
						Composite->SetCompositeLength(Composite->AnimationTrack.GetLength());
						Composite->PostEditChange();
						Composite->MarkPackageDirty();
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AppendTrack(Data, *Track);
					Data->SetNumberField(TEXT("length"),
						Cast<UAnimSequenceBase>(Asset) != nullptr
							? Cast<UAnimSequenceBase>(Asset)->GetPlayLength()
							: 0.0);
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------- blend space edits ---
				if (Operation == TEXT("add_sample") || Operation == TEXT("set_sample")
					|| Operation == TEXT("remove_sample") || Operation == TEXT("set_axis"))
				{
					UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset);
					if (BlendSpace == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrong_asset_type"),
							FString::Printf(TEXT("'%s' is a %s — samples and axes are blend-space only"),
								*Asset->GetPathName(), *Asset->GetClass()->GetName()));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditBlendSpace", "McpLink Edit Blend Space"));
					BlendSpace->Modify();

					if (Operation == TEXT("set_axis"))
					{
						const int32 Axis = IntOr(Body, TEXT("axis"), 0);
						if (Axis < 0 || Axis > 2)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_axis"),
								TEXT("'axis' must be 0 (horizontal) or 1 (vertical)"));
							return;
						}
						FBlendParameter* Found = MutableBlendParameter(BlendSpace, Axis);
						if (Found == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_property"),
								TEXT("UBlendSpace::BlendParameters is not reflected in this engine build"));
							return;
						}
						FBlendParameter& Parameter = *Found;
						FString Name;
						if (Body->TryGetStringField(TEXT("name"), Name))
						{
							Parameter.DisplayName = Name;
						}
						Parameter.Min = DoubleOr(Body, TEXT("min"), Parameter.Min);
						Parameter.Max = DoubleOr(Body, TEXT("max"), Parameter.Max);
						Parameter.GridNum = IntOr(Body, TEXT("grid_divisions"), Parameter.GridNum);
						BlendSpace->PostEditChange();
						BlendSpace->MarkPackageDirty();
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetNumberField(TEXT("axis"), Axis);
						Data->SetStringField(TEXT("name"), Parameter.DisplayName);
						Data->SetNumberField(TEXT("min"), Parameter.Min);
						Data->SetNumberField(TEXT("max"), Parameter.Max);
						Data->SetNumberField(TEXT("grid_divisions"), Parameter.GridNum);
						Responder->Ok(Data);
						return;
					}

					FVector Position = FVector::ZeroVector;
					const bool bHasPosition = GetVector(Body, TEXT("position"), Position);

					if (Operation == TEXT("add_sample"))
					{
						FString AnimPath;
						if (!RequireString(Body, TEXT("animation"), AnimPath, Responder,
								TEXT("an Anim Sequence path")))
						{
							return;
						}
						UAnimSequence* Sequence = Cast<UAnimSequence>(ResolveAsset(AnimPath));
						if (Sequence == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
								FString::Printf(TEXT("no Anim Sequence at '%s'"), *AnimPath));
							return;
						}
						if (!bHasPosition)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'position' is required, e.g. [0, 0, 0] — X is the first axis"));
							return;
						}
						if (!BlendSpace->ValidateSampleValue(Position))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_position"),
								FString::Printf(
									TEXT("position [%g, %g] is outside the axis ranges or duplicates a sample ")
									TEXT("— check axes with info"),
									Position.X, Position.Y));
							return;
						}
						const int32 Index = BlendSpace->AddSample(Sequence, Position);
						if (Index == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
								TEXT("the blend space refused the sample (skeleton mismatch?)"));
							return;
						}
						BlendSpace->PostEditChange();
						BlendSpace->MarkPackageDirty();
						Responder->Ok(BlendSampleToJson(BlendSpace->GetBlendSample(Index), Index));
						return;
					}

					const int32 Index = IntOr(Body, TEXT("index"), -1);
					if (!BlendSpace->GetBlendSamples().IsValidIndex(Index))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_index"),
							FString::Printf(TEXT("'index' must be 0..%d"),
								BlendSpace->GetBlendSamples().Num() - 1));
						return;
					}
					if (Operation == TEXT("set_sample"))
					{
						if (!bHasPosition || !BlendSpace->EditSampleValue(Index, Position))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_position"),
								TEXT("'position' is required and must be inside the axis ranges"));
							return;
						}
						BlendSpace->PostEditChange();
						BlendSpace->MarkPackageDirty();
						Responder->Ok(BlendSampleToJson(BlendSpace->GetBlendSample(Index), Index));
						return;
					}

					if (!BlendSpace->DeleteSample(Index))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("remove_failed"),
							TEXT("the blend space refused to delete that sample"));
						return;
					}
					BlendSpace->PostEditChange();
					BlendSpace->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("removed"), Index);
					Data->SetNumberField(TEXT("sample_count"), BlendSpace->GetBlendSamples().Num());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("unhandled_operation"),
					FString::Printf(TEXT("operation '%s' is recognised but not implemented"), *Operation));
			});
	}
}
