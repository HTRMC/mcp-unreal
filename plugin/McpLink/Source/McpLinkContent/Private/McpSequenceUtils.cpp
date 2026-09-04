#include "McpSequenceUtils.h"

#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Channels/MovieSceneObjectPathChannel.h"
#include "Channels/MovieSceneStringChannel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "LevelSequence.h"
#include "McpAssetUtils.h"
#include "McpResolve.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTimeHelpers.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneDoubleTrack.h"
#include "Tracks/MovieSceneEnumTrack.h"
#include "Tracks/MovieSceneEulerTransformTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneIntegerTrack.h"
#include "Tracks/MovieSceneObjectPropertyTrack.h"
#include "Tracks/MovieSceneRotatorTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "Tracks/MovieSceneStringTrack.h"
#include "Tracks/MovieSceneTextTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "WidgetBlueprint.h"

namespace McpLink::Sequences
{
	namespace
	{
		/// What a channel is addressed by. Single-channel sections (a bool, a
		/// float property) leave the metadata name unset, which would otherwise
		/// surface as the literal "None".
		FString ChannelName(const TArrayView<const FMovieSceneChannelMetaData>& MetaData, int32 Index)
		{
			if (MetaData.IsValidIndex(Index) && !MetaData[Index].Name.IsNone())
			{
				return MetaData[Index].Name.ToString();
			}
			return TEXT("Value");
		}

		double AsNumber(const TSharedPtr<FJsonValue>& Value, bool& bOk)
		{
			double Out = 0.0;
			bOk = Value.IsValid() && Value->TryGetNumber(Out);
			return Out;
		}
	}

	UMovieSceneSequence* Find(const FString& Spec)
	{
		if (Spec.IsEmpty())
		{
			return nullptr;
		}

		// "/Game/UI/WBP_Menu:Anim_FadeIn" — the asset name is implied, the way it
		// is for a Level Sequence path.
		FString AssetPath = Spec;
		FString SubObject;
		if (Spec.Split(TEXT(":"), &AssetPath, &SubObject) && !AssetPath.Contains(TEXT(".")))
		{
			const FString Expanded = FString::Printf(TEXT("%s.%s:%s"),
				*AssetPath, *FPackageName::GetShortName(AssetPath), *SubObject);
			if (UObject* Sub = ResolveObject(Expanded))
			{
				return Cast<UMovieSceneSequence>(Sub);
			}
		}

		UObject* Object = ResolveObject(Spec);
		if (Object == nullptr && !Spec.Contains(TEXT(".")))
		{
			Object = ResolveObject(FString::Printf(TEXT("%s.%s"), *Spec, *FPackageName::GetShortName(Spec)));
		}
		return Cast<UMovieSceneSequence>(Object);
	}

	UWidgetAnimation* CreateWidgetAnimation(
		UWidgetBlueprint* Blueprint,
		const FString& Name,
		FFrameRate DisplayRate,
		double DurationSeconds,
		FString& OutError)
	{
		if (Blueprint == nullptr)
		{
			OutError = TEXT("no Widget Blueprint");
			return nullptr;
		}
		// The name becomes the object name and a Blueprint variable.
		if (!FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a valid animation name — use letters, digits and underscores"), *Name);
			return nullptr;
		}
		for (const UWidgetAnimation* Existing : Blueprint->Animations)
		{
			if (Existing != nullptr && Existing->GetFName() == FName(*Name))
			{
				OutError = FString::Printf(TEXT("'%s' already has an animation called '%s'"),
					*Blueprint->GetName(), *Name);
				return nullptr;
			}
		}

		Blueprint->Modify();
		UWidgetAnimation* Animation = NewObject<UWidgetAnimation>(Blueprint, FName(*Name), RF_Transactional);
		Animation->SetDisplayLabel(Name);
		Animation->MovieScene = NewObject<UMovieScene>(Animation, FName(*Name), RF_Transactional);
		Animation->MovieScene->SetDisplayRate(DisplayRate);
		// The UMG panel adds a tick past the end here; a plain [0, duration) range
		// keeps the reported end on a whole display frame and plays the same.
		const FFrameNumber End =
			(FMath::Max(DurationSeconds, 0.0) * Animation->MovieScene->GetTickResolution()).RoundToFrame();
		Animation->MovieScene->SetPlaybackRange(
			TRange<FFrameNumber>(FFrameNumber(0), FMath::Max(End, FFrameNumber(1))));

		Blueprint->Animations.Add(Animation);
		Blueprint->OnVariableAdded(Animation->GetFName());
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return Animation;
	}

	FGuid BindWidget(UWidgetAnimation& Animation, const FString& WidgetName, FString& OutError)
	{
		UWidgetBlueprint* Blueprint = Animation.GetTypedOuter<UWidgetBlueprint>();
		if (Blueprint == nullptr || Blueprint->WidgetTree == nullptr)
		{
			OutError = TEXT("this animation is not owned by a Widget Blueprint");
			return FGuid();
		}
		// BindPossessableObject needs a UUserWidget for context and compares the
		// possessed object against it to spot the root binding, so the generated
		// class's default object stands in for the editor's preview widget.
		UUserWidget* Context = Blueprint->GeneratedClass != nullptr
			? Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>()
			: nullptr;
		if (Context == nullptr)
		{
			OutError = TEXT("the Widget Blueprint has not been compiled yet");
			return FGuid();
		}

		UObject* ToBind = WidgetName.Equals(TEXT("Self"), ESearchCase::IgnoreCase)
			? static_cast<UObject*>(Context)
			: Blueprint->WidgetTree->FindWidget(FName(*WidgetName));
		if (ToBind == nullptr)
		{
			TArray<FString> Names;
			Blueprint->WidgetTree->ForEachWidget([&Names](UWidget* Widget)
			{
				if (Widget != nullptr) { Names.Add(Widget->GetName()); }
			});
			OutError = FString::Printf(TEXT("'%s' is not a widget of %s — it has: %s"),
				*WidgetName, *Blueprint->GetName(), *FString::Join(Names, TEXT(", ")));
			return FGuid();
		}

		UMovieScene* MovieScene = Animation.GetMovieScene();
		for (const FWidgetAnimationBinding& Binding : Animation.GetBindings())
		{
			if (Binding.WidgetName == ToBind->GetFName() && MovieScene->FindBinding(Binding.AnimationGuid))
			{
				return Binding.AnimationGuid;
			}
		}

		Animation.Modify();
		MovieScene->Modify();
		const FGuid Guid = MovieScene->AddPossessable(ToBind->GetName(), ToBind->GetClass());
		Animation.BindPossessableObject(Guid, *ToBind, Context);
		return Guid;
	}

	ULevelSequence* Create(
		const FString& PackagePath, FFrameRate DisplayRate, FFrameRate TickResolution, FString& OutError)
	{
		// A null factory makes IAssetTools NewObject the class directly, which is
		// what ULevelSequenceFactoryNew does — minus Initialize() and the default
		// ranges, applied here so the asset opens in Sequencer like any other.
		UObject* Asset = CreateAsset(PackagePath, ULevelSequence::StaticClass(), nullptr, OutError);
		ULevelSequence* Sequence = Cast<ULevelSequence>(Asset);
		if (Sequence == nullptr)
		{
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("could not create a Level Sequence at '%s'"), *PackagePath);
			}
			return nullptr;
		}

		Sequence->Initialize();
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		MovieScene->SetTickResolutionDirectly(TickResolution);
		MovieScene->SetDisplayRate(DisplayRate);
		return Sequence;
	}

	FFrameNumber TickFromDisplayFrame(const UMovieScene& MovieScene, double DisplayFrame)
	{
		const FFrameTime Time = FFrameRate::TransformTime(
			FFrameTime::FromDecimal(DisplayFrame), MovieScene.GetDisplayRate(), MovieScene.GetTickResolution());
		return Time.RoundToFrame();
	}

	FFrameNumber TickFromSeconds(const UMovieScene& MovieScene, double Seconds)
	{
		return (Seconds * MovieScene.GetTickResolution()).RoundToFrame();
	}

	double DisplayFrameFromTick(const UMovieScene& MovieScene, FFrameNumber Tick)
	{
		return FFrameRate::TransformTime(
			FFrameTime(Tick), MovieScene.GetTickResolution(), MovieScene.GetDisplayRate()).AsDecimal();
	}

	double SecondsFromTick(const UMovieScene& MovieScene, FFrameNumber Tick)
	{
		return MovieScene.GetTickResolution().AsSeconds(FFrameTime(Tick));
	}

	bool ReadTime(
		const TSharedRef<FJsonObject>& Body,
		const UMovieScene& MovieScene,
		const TCHAR* FrameField,
		const TCHAR* SecondsField,
		FFrameNumber& OutTick)
	{
		double Number = 0.0;
		if (Body->TryGetNumberField(FrameField, Number))
		{
			OutTick = TickFromDisplayFrame(MovieScene, Number);
			return true;
		}
		if (Body->TryGetNumberField(SecondsField, Number))
		{
			OutTick = TickFromSeconds(MovieScene, Number);
			return true;
		}
		return false;
	}

	TRange<FFrameNumber> ReadRange(const TSharedRef<FJsonObject>& Body, const UMovieScene& MovieScene)
	{
		const TRange<FFrameNumber> Playback = MovieScene.GetPlaybackRange();
		FFrameNumber Start = UE::MovieScene::DiscreteInclusiveLower(Playback);
		FFrameNumber End = UE::MovieScene::DiscreteExclusiveUpper(Playback);
		ReadTime(Body, MovieScene, TEXT("start_frame"), TEXT("start_seconds"), Start);
		ReadTime(Body, MovieScene, TEXT("end_frame"), TEXT("end_seconds"), End);
		if (End <= Start)
		{
			End = Start + 1;
		}
		return TRange<FFrameNumber>(Start, End);
	}

	FGuid FindBinding(const UMovieScene& MovieScene, const FString& Spec)
	{
		FGuid Guid;
		if (FGuid::Parse(Spec, Guid) && MovieScene.FindBinding(Guid) != nullptr)
		{
			return Guid;
		}
		for (const FMovieSceneBinding& Binding : MovieScene.GetBindings())
		{
			if (MovieScene.GetObjectDisplayName(Binding.GetObjectGuid()).ToString() == Spec)
			{
				return Binding.GetObjectGuid();
			}
		}
		return FGuid();
	}

	UMovieSceneTrack* FindTrack(const UMovieScene& MovieScene, const FString& Spec)
	{
		if (Spec.IsEmpty())
		{
			return nullptr;
		}
		TArray<UMovieSceneTrack*> All(MovieScene.GetTracks());
		for (const FMovieSceneBinding& Binding : MovieScene.GetBindings())
		{
			All.Append(Binding.GetTracks());
		}
		if (UMovieSceneTrack* CameraCut = MovieScene.GetCameraCutTrack())
		{
			All.AddUnique(CameraCut);
		}
		for (UMovieSceneTrack* Track : All)
		{
			if (Track != nullptr && (Track->GetPathName() == Spec || Track->GetName() == Spec))
			{
				return Track;
			}
		}
		return nullptr;
	}

	UMovieSceneSection* FindSection(
		const UMovieScene& MovieScene, const FString& Spec, TArray<FString>& OutAmbiguous)
	{
		OutAmbiguous.Reset();
		if (Spec.IsEmpty())
		{
			return nullptr;
		}
		UMovieSceneSection* ByName = nullptr;
		for (UMovieSceneSection* Section : MovieScene.GetAllSections())
		{
			if (Section == nullptr)
			{
				continue;
			}
			if (Section->GetPathName() == Spec)
			{
				OutAmbiguous.Reset();
				return Section;
			}
			if (Section->GetName() == Spec)
			{
				OutAmbiguous.Add(Section->GetPathName());
				ByName = Section;
			}
		}
		// Section names restart per track, so a bare name can hit more than one.
		return OutAmbiguous.Num() == 1 ? ByName : nullptr;
	}

	UClass* ResolveTrackClass(const FString& Spec)
	{
		if (Spec.IsEmpty())
		{
			return nullptr;
		}
		// "Transform" would otherwise land on UMovieSceneTransformTrack, the
		// property track for FTransform properties, rather than the actor
		// transform track Sequencer shows under that name.
		if (Spec.Equals(TEXT("Transform"), ESearchCase::IgnoreCase)
			|| Spec.Equals(TEXT("3DTransform"), ESearchCase::IgnoreCase))
		{
			return UMovieScene3DTransformTrack::StaticClass();
		}

		UClass* Class = ResolveClass(Spec);
		if (Class == nullptr)
		{
			const FString Full = FString::Printf(TEXT("MovieScene%sTrack"), *Spec);
			Class = ResolveClass(Full);
		}
		if (Class == nullptr)
		{
			Class = ResolveClass(FString::Printf(TEXT("MovieScene3D%sTrack"), *Spec));
		}
		if (Class != nullptr && Class->IsChildOf(UMovieSceneTrack::StaticClass())
			&& !Class->HasAnyClassFlags(CLASS_Abstract))
		{
			return Class;
		}
		return nullptr;
	}

	UClass* PropertyTrackClass(
		const FProperty* Property, int32& OutNumChannels, UEnum*& OutEnum, UClass*& OutObjectClass)
	{
		OutNumChannels = 0;
		OutEnum = nullptr;
		OutObjectClass = nullptr;
		if (Property == nullptr)
		{
			return nullptr;
		}

		if (CastField<FBoolProperty>(Property) != nullptr)
		{
			return UMovieSceneBoolTrack::StaticClass();
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			OutEnum = EnumProperty->GetEnum();
			return UMovieSceneEnumTrack::StaticClass();
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			OutEnum = ByteProperty->Enum;
			return OutEnum != nullptr
				? UMovieSceneEnumTrack::StaticClass()
				: UMovieSceneByteTrack::StaticClass();
		}
		if (CastField<FIntProperty>(Property) != nullptr)
		{
			return UMovieSceneIntegerTrack::StaticClass();
		}
		if (CastField<FFloatProperty>(Property) != nullptr)
		{
			return UMovieSceneFloatTrack::StaticClass();
		}
		if (CastField<FDoubleProperty>(Property) != nullptr)
		{
			return UMovieSceneDoubleTrack::StaticClass();
		}
		if (CastField<FStrProperty>(Property) != nullptr)
		{
			return UMovieSceneStringTrack::StaticClass();
		}
		if (CastField<FTextProperty>(Property) != nullptr)
		{
			return UMovieSceneTextTrack::StaticClass();
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			OutObjectClass = ObjectProperty->PropertyClass;
			return UMovieSceneObjectPropertyTrack::StaticClass();
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const FName StructName = StructProperty->Struct->GetFName();
			if (StructName == NAME_Vector || StructName == NAME_Vector3d)
			{
				OutNumChannels = 3;
				return UMovieSceneDoubleVectorTrack::StaticClass();
			}
			if (StructName == NAME_Vector2D)
			{
				OutNumChannels = 2;
				return UMovieSceneDoubleVectorTrack::StaticClass();
			}
			if (StructName == NAME_Vector4)
			{
				OutNumChannels = 4;
				return UMovieSceneDoubleVectorTrack::StaticClass();
			}
			if (StructName == NAME_Vector3f)
			{
				OutNumChannels = 3;
				return UMovieSceneFloatVectorTrack::StaticClass();
			}
			if (StructName == NAME_Vector2f)
			{
				OutNumChannels = 2;
				return UMovieSceneFloatVectorTrack::StaticClass();
			}
			if (StructName == NAME_Vector4f)
			{
				OutNumChannels = 4;
				return UMovieSceneFloatVectorTrack::StaticClass();
			}
			if (StructName == NAME_LinearColor || StructName == NAME_Color || StructName == FName(TEXT("SlateColor")))
			{
				return UMovieSceneColorTrack::StaticClass();
			}
			if (StructName == NAME_Rotator || StructName == NAME_Rotator3d)
			{
				return UMovieSceneRotatorTrack::StaticClass();
			}
			if (StructName == NAME_Transform)
			{
				return UMovieScene3DTransformTrack::StaticClass();
			}
			if (StructName == FName(TEXT("EulerTransform")))
			{
				return UMovieSceneEulerTransformTrack::StaticClass();
			}
		}
		return nullptr;
	}

	FMovieSceneChannel* FindChannel(UMovieSceneSection& Section, const FString& Name, FName& OutTypeName)
	{
		OutTypeName = NAME_None;
		const FMovieSceneChannelProxy& Proxy = Section.GetChannelProxy();
		// An empty request means "the only channel", which is how a single-value
		// section (bool, float property) is usually addressed.
		const bool bTakeFirst = Name.IsEmpty() && Proxy.NumChannels() == 1;
		for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
		{
			const TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
			const TArrayView<const FMovieSceneChannelMetaData> MetaData = Entry.GetMetaData();
			for (int32 Index = 0; Index < Channels.Num(); ++Index)
			{
				if (bTakeFirst || ChannelName(MetaData, Index) == Name)
				{
					OutTypeName = Entry.GetChannelTypeName();
					return Channels[Index];
				}
			}
		}
		return nullptr;
	}

	bool SetKey(
		FMovieSceneChannel* Channel,
		FName TypeName,
		FFrameNumber Tick,
		const TSharedPtr<FJsonValue>& Value,
		const FString& Interpolation,
		FString& OutError)
	{
		if (Channel == nullptr)
		{
			OutError = TEXT("no channel");
			return false;
		}

		bool bNumber = false;
		const double Number = AsNumber(Value, bNumber);
		const FString Interp = Interpolation.IsEmpty() ? TEXT("auto") : Interpolation.ToLower();

		if (TypeName == FMovieSceneDoubleChannel::StaticStruct()->GetFName()
			|| TypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName())
		{
			if (!bNumber)
			{
				OutError = TEXT("this channel takes a number");
				return false;
			}
			const bool bDouble = TypeName == FMovieSceneDoubleChannel::StaticStruct()->GetFName();
			if (Interp == TEXT("linear"))
			{
				bDouble ? static_cast<FMovieSceneDoubleChannel*>(Channel)->AddLinearKey(Tick, Number)
						: static_cast<FMovieSceneFloatChannel*>(Channel)->AddLinearKey(Tick, Number);
			}
			else if (Interp == TEXT("constant"))
			{
				bDouble ? static_cast<FMovieSceneDoubleChannel*>(Channel)->AddConstantKey(Tick, Number)
						: static_cast<FMovieSceneFloatChannel*>(Channel)->AddConstantKey(Tick, Number);
			}
			else if (Interp == TEXT("auto") || Interp == TEXT("cubic"))
			{
				bDouble ? static_cast<FMovieSceneDoubleChannel*>(Channel)->AddCubicKey(Tick, Number, RCTM_Auto)
						: static_cast<FMovieSceneFloatChannel*>(Channel)->AddCubicKey(Tick, Number, RCTM_Auto);
			}
			else
			{
				OutError = FString::Printf(
					TEXT("unknown interpolation '%s' — use auto, linear, constant or cubic"), *Interpolation);
				return false;
			}
			return true;
		}
		if (TypeName == FMovieSceneBoolChannel::StaticStruct()->GetFName())
		{
			bool bValue = false;
			if (!Value.IsValid() || !Value->TryGetBool(bValue))
			{
				OutError = TEXT("this channel takes true or false");
				return false;
			}
			static_cast<FMovieSceneBoolChannel*>(Channel)->GetData().UpdateOrAddKey(Tick, bValue);
			return true;
		}
		if (TypeName == FMovieSceneIntegerChannel::StaticStruct()->GetFName())
		{
			if (!bNumber)
			{
				OutError = TEXT("this channel takes a whole number");
				return false;
			}
			static_cast<FMovieSceneIntegerChannel*>(Channel)->GetData()
				.UpdateOrAddKey(Tick, static_cast<int32>(Number));
			return true;
		}
		if (TypeName == FMovieSceneByteChannel::StaticStruct()->GetFName())
		{
			// Enum channels accept the enumerator name as well as its value.
			uint8 Byte = 0;
			FString AsText;
			if (bNumber)
			{
				Byte = static_cast<uint8>(Number);
			}
			else if (Value.IsValid() && Value->TryGetString(AsText))
			{
				const UEnum* Enum = static_cast<FMovieSceneByteChannel*>(Channel)->GetEnum();
				const int64 Found = Enum != nullptr ? Enum->GetValueByNameString(AsText) : INDEX_NONE;
				if (Found == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("'%s' is not a value of this channel's enum"), *AsText);
					return false;
				}
				Byte = static_cast<uint8>(Found);
			}
			else
			{
				OutError = TEXT("this channel takes a number or an enumerator name");
				return false;
			}
			static_cast<FMovieSceneByteChannel*>(Channel)->GetData().UpdateOrAddKey(Tick, Byte);
			return true;
		}
		if (TypeName == FMovieSceneStringChannel::StaticStruct()->GetFName())
		{
			FString AsText;
			if (!Value.IsValid() || !Value->TryGetString(AsText))
			{
				OutError = TEXT("this channel takes a string");
				return false;
			}
			static_cast<FMovieSceneStringChannel*>(Channel)->GetData().UpdateOrAddKey(Tick, AsText);
			return true;
		}
		if (TypeName == FMovieSceneObjectPathChannel::StaticStruct()->GetFName())
		{
			FString Path;
			if (!Value.IsValid() || !Value->TryGetString(Path))
			{
				OutError = TEXT("this channel takes an object path");
				return false;
			}
			UObject* Object = ResolveObject(Path);
			if (Object == nullptr)
			{
				OutError = FString::Printf(TEXT("no object at '%s'"), *Path);
				return false;
			}
			static_cast<FMovieSceneObjectPathChannel*>(Channel)->GetData()
				.UpdateOrAddKey(Tick, FMovieSceneObjectPathChannelKeyValue(Object));
			return true;
		}

		OutError = FString::Printf(
			TEXT("keys on '%s' channels cannot be set through sequence_ops — edit the section's ")
			TEXT("properties with set_property instead"),
			*TypeName.ToString());
		return false;
	}

	bool SetChannelDefault(
		FMovieSceneChannel* Channel, FName TypeName, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (Channel == nullptr)
		{
			OutError = TEXT("no channel");
			return false;
		}
		bool bNumber = false;
		const double Number = AsNumber(Value, bNumber);

		if (TypeName == FMovieSceneDoubleChannel::StaticStruct()->GetFName() && bNumber)
		{
			static_cast<FMovieSceneDoubleChannel*>(Channel)->SetDefault(Number);
			return true;
		}
		if (TypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName() && bNumber)
		{
			static_cast<FMovieSceneFloatChannel*>(Channel)->SetDefault(static_cast<float>(Number));
			return true;
		}
		if (TypeName == FMovieSceneIntegerChannel::StaticStruct()->GetFName() && bNumber)
		{
			static_cast<FMovieSceneIntegerChannel*>(Channel)->SetDefault(static_cast<int32>(Number));
			return true;
		}
		if (TypeName == FMovieSceneByteChannel::StaticStruct()->GetFName() && bNumber)
		{
			static_cast<FMovieSceneByteChannel*>(Channel)->SetDefault(static_cast<uint8>(Number));
			return true;
		}
		bool bValue = false;
		if (TypeName == FMovieSceneBoolChannel::StaticStruct()->GetFName()
			&& Value.IsValid() && Value->TryGetBool(bValue))
		{
			static_cast<FMovieSceneBoolChannel*>(Channel)->SetDefault(bValue);
			return true;
		}
		FString AsText;
		if (TypeName == FMovieSceneStringChannel::StaticStruct()->GetFName()
			&& Value.IsValid() && Value->TryGetString(AsText))
		{
			static_cast<FMovieSceneStringChannel*>(Channel)->SetDefault(AsText);
			return true;
		}

		OutError = FString::Printf(
			TEXT("no default can be set for a '%s' channel from that value"), *TypeName.ToString());
		return false;
	}

	TSharedRef<FJsonObject> RangeToJson(const UMovieScene& MovieScene, const TRange<FFrameNumber>& Range)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		if (Range.GetLowerBound().IsOpen() || Range.GetUpperBound().IsOpen())
		{
			Json->SetBoolField(TEXT("infinite"), true);
			return Json;
		}
		const FFrameNumber Start = UE::MovieScene::DiscreteInclusiveLower(Range);
		const FFrameNumber End = UE::MovieScene::DiscreteExclusiveUpper(Range);
		Json->SetNumberField(TEXT("start_frame"), DisplayFrameFromTick(MovieScene, Start));
		Json->SetNumberField(TEXT("end_frame"), DisplayFrameFromTick(MovieScene, End));
		Json->SetNumberField(TEXT("start_seconds"), SecondsFromTick(MovieScene, Start));
		Json->SetNumberField(TEXT("end_seconds"), SecondsFromTick(MovieScene, End));
		Json->SetNumberField(TEXT("start_tick"), Start.Value);
		Json->SetNumberField(TEXT("end_tick"), End.Value);
		return Json;
	}

	TSharedRef<FJsonObject> ChannelToJson(
		const UMovieScene& MovieScene, const FMovieSceneChannel& Channel, FName TypeName,
		const FString& Name, bool bIncludeKeys)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Name);
		Json->SetStringField(TEXT("type"), TypeName.ToString());
		Json->SetNumberField(TEXT("num_keys"), Channel.GetNumKeys());
		if (!bIncludeKeys || Channel.GetNumKeys() == 0)
		{
			return Json;
		}

		TArray<FFrameNumber> Times;
		const_cast<FMovieSceneChannel&>(Channel).GetKeys(TRange<FFrameNumber>::All(), &Times, nullptr);

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (int32 Index = 0; Index < Times.Num(); ++Index)
		{
			const TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
			Key->SetNumberField(TEXT("frame"), DisplayFrameFromTick(MovieScene, Times[Index]));
			Key->SetNumberField(TEXT("seconds"), SecondsFromTick(MovieScene, Times[Index]));
			Key->SetNumberField(TEXT("tick"), Times[Index].Value);

			if (TypeName == FMovieSceneDoubleChannel::StaticStruct()->GetFName())
			{
				const auto& Values = static_cast<const FMovieSceneDoubleChannel&>(Channel).GetValues();
				if (Values.IsValidIndex(Index)) { Key->SetNumberField(TEXT("value"), Values[Index].Value); }
			}
			else if (TypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName())
			{
				const auto& Values = static_cast<const FMovieSceneFloatChannel&>(Channel).GetValues();
				if (Values.IsValidIndex(Index)) { Key->SetNumberField(TEXT("value"), Values[Index].Value); }
			}
			else if (TypeName == FMovieSceneBoolChannel::StaticStruct()->GetFName())
			{
				const auto& Values = static_cast<const FMovieSceneBoolChannel&>(Channel).GetValues();
				if (Values.IsValidIndex(Index)) { Key->SetBoolField(TEXT("value"), Values[Index]); }
			}
			else if (TypeName == FMovieSceneIntegerChannel::StaticStruct()->GetFName())
			{
				const auto& Values = static_cast<const FMovieSceneIntegerChannel&>(Channel).GetValues();
				if (Values.IsValidIndex(Index)) { Key->SetNumberField(TEXT("value"), Values[Index]); }
			}
			else if (TypeName == FMovieSceneByteChannel::StaticStruct()->GetFName())
			{
				const auto& Values = static_cast<const FMovieSceneByteChannel&>(Channel).GetValues();
				if (Values.IsValidIndex(Index)) { Key->SetNumberField(TEXT("value"), Values[Index]); }
			}
			else if (TypeName == FMovieSceneStringChannel::StaticStruct()->GetFName())
			{
				const auto Values = static_cast<const FMovieSceneStringChannel&>(Channel).GetData().GetValues();
				if (Values.IsValidIndex(Index)) { Key->SetStringField(TEXT("value"), Values[Index]); }
			}
			Keys.Add(MakeShared<FJsonValueObject>(Key));
		}
		Json->SetArrayField(TEXT("keys"), Keys);
		return Json;
	}

	TSharedRef<FJsonObject> SectionToJson(
		const UMovieScene& MovieScene, UMovieSceneSection& Section, bool bIncludeKeys)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("path"), Section.GetPathName());
		Json->SetStringField(TEXT("name"), Section.GetName());
		Json->SetStringField(TEXT("class"), Section.GetClass()->GetName());
		Json->SetNumberField(TEXT("row_index"), Section.GetRowIndex());
		Json->SetBoolField(TEXT("active"), Section.IsActive());
		Json->SetObjectField(TEXT("range"), RangeToJson(MovieScene, Section.GetRange()));

		TArray<TSharedPtr<FJsonValue>> Channels;
		for (const FMovieSceneChannelEntry& Entry : Section.GetChannelProxy().GetAllEntries())
		{
			const TArrayView<FMovieSceneChannel* const> Handles = Entry.GetChannels();
			const TArrayView<const FMovieSceneChannelMetaData> MetaData = Entry.GetMetaData();
			for (int32 Index = 0; Index < Handles.Num(); ++Index)
			{
				Channels.Add(MakeShared<FJsonValueObject>(ChannelToJson(
					MovieScene, *Handles[Index], Entry.GetChannelTypeName(),
					ChannelName(MetaData, Index), bIncludeKeys)));
			}
		}
		Json->SetArrayField(TEXT("channels"), Channels);
		return Json;
	}

	TSharedRef<FJsonObject> TrackToJson(
		const UMovieScene& MovieScene, UMovieSceneTrack& Track, bool bIncludeKeys)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("path"), Track.GetPathName());
		Json->SetStringField(TEXT("name"), Track.GetName());
		Json->SetStringField(TEXT("class"), Track.GetClass()->GetName());
		Json->SetStringField(TEXT("display_name"), Track.GetDisplayName().ToString());
		if (const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(&Track))
		{
			Json->SetStringField(TEXT("property"), PropertyTrack->GetPropertyPath().ToString());
		}

		TArray<TSharedPtr<FJsonValue>> Sections;
		for (UMovieSceneSection* Section : Track.GetAllSections())
		{
			if (Section != nullptr)
			{
				Sections.Add(MakeShared<FJsonValueObject>(SectionToJson(MovieScene, *Section, bIncludeKeys)));
			}
		}
		Json->SetArrayField(TEXT("sections"), Sections);
		return Json;
	}

	TSharedRef<FJsonObject> BindingToJson(const UMovieScene& MovieScene, const FGuid& Guid, bool bIncludeKeys)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("guid"), Guid.ToString(EGuidFormats::DigitsWithHyphens));
		Json->SetStringField(TEXT("name"), MovieScene.GetObjectDisplayName(Guid).ToString());

		// 5.8 spawnables are possessables carrying a custom binding, so the spawn
		// track — not FindSpawnable — is what says a binding spawns its object.
		const bool bSpawns =
			MovieScene.FindTrack(UMovieSceneSpawnTrack::StaticClass(), Guid) != nullptr;
		UMovieScene& Mutable = const_cast<UMovieScene&>(MovieScene);
		if (const FMovieScenePossessable* Possessable = Mutable.FindPossessable(Guid))
		{
			Json->SetStringField(TEXT("kind"), bSpawns ? TEXT("spawnable") : TEXT("possessable"));
			if (const UClass* Class = Possessable->GetPossessedObjectClass())
			{
				Json->SetStringField(TEXT("class"), Class->GetPathName());
			}
			if (Possessable->GetParent().IsValid())
			{
				Json->SetStringField(
					TEXT("parent_guid"), Possessable->GetParent().ToString(EGuidFormats::DigitsWithHyphens));
			}
		}
		else if (const FMovieSceneSpawnable* Spawnable = Mutable.FindSpawnable(Guid))
		{
			Json->SetStringField(TEXT("kind"), TEXT("spawnable"));
			if (const UObject* Template = Spawnable->GetObjectTemplate())
			{
				Json->SetStringField(TEXT("class"), Template->GetClass()->GetPathName());
			}
		}
		else
		{
			Json->SetStringField(TEXT("kind"), bSpawns ? TEXT("spawnable") : TEXT("binding"));
		}

		if (const UWidgetAnimation* Animation = MovieScene.GetTypedOuter<UWidgetAnimation>())
		{
			for (const FWidgetAnimationBinding& Bound : Animation->GetBindings())
			{
				if (Bound.AnimationGuid == Guid)
				{
					Json->SetStringField(TEXT("widget"), Bound.WidgetName.ToString());
					Json->SetBoolField(TEXT("root_widget"), Bound.bIsRootWidget);
					break;
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> Tracks;
		if (const FMovieSceneBinding* Binding = MovieScene.FindBinding(Guid))
		{
			for (UMovieSceneTrack* Track : Binding->GetTracks())
			{
				if (Track != nullptr)
				{
					Tracks.Add(MakeShared<FJsonValueObject>(TrackToJson(MovieScene, *Track, bIncludeKeys)));
				}
			}
		}
		Json->SetArrayField(TEXT("tracks"), Tracks);
		return Json;
	}

	TSharedRef<FJsonObject> SequenceToJson(UMovieSceneSequence& Sequence, bool bIncludeKeys)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("sequence"), Sequence.GetPathName());
		UMovieScene* MovieScene = Sequence.GetMovieScene();
		if (MovieScene == nullptr)
		{
			return Json;
		}
		if (const UWidgetAnimation* Animation = Cast<UWidgetAnimation>(&Sequence))
		{
			Json->SetStringField(TEXT("kind"), TEXT("widget_animation"));
			Json->SetStringField(TEXT("name"), Animation->GetName());
			if (const UWidgetBlueprint* Blueprint = Animation->GetTypedOuter<UWidgetBlueprint>())
			{
				Json->SetStringField(TEXT("widget_blueprint"), Blueprint->GetPathName());
			}
		}
		else
		{
			Json->SetStringField(TEXT("kind"), TEXT("level_sequence"));
		}

		Json->SetNumberField(TEXT("display_rate"), MovieScene->GetDisplayRate().AsDecimal());
		Json->SetNumberField(TEXT("tick_resolution"), MovieScene->GetTickResolution().AsDecimal());
		Json->SetObjectField(TEXT("playback_range"), RangeToJson(*MovieScene, MovieScene->GetPlaybackRange()));

		TArray<TSharedPtr<FJsonValue>> Marks;
		for (const FMovieSceneMarkedFrame& Mark : MovieScene->GetMarkedFrames())
		{
			const TSharedRef<FJsonObject> MarkJson = MakeShared<FJsonObject>();
			MarkJson->SetStringField(TEXT("label"), Mark.Label);
			MarkJson->SetNumberField(TEXT("frame"), DisplayFrameFromTick(*MovieScene, Mark.FrameNumber));
			MarkJson->SetNumberField(TEXT("seconds"), SecondsFromTick(*MovieScene, Mark.FrameNumber));
			Marks.Add(MakeShared<FJsonValueObject>(MarkJson));
		}
		Json->SetArrayField(TEXT("marked_frames"), Marks);

		TArray<TSharedPtr<FJsonValue>> RootTracks;
		for (UMovieSceneTrack* Track : MovieScene->GetTracks())
		{
			if (Track != nullptr)
			{
				RootTracks.Add(MakeShared<FJsonValueObject>(TrackToJson(*MovieScene, *Track, bIncludeKeys)));
			}
		}
		if (UMovieSceneTrack* CameraCut = MovieScene->GetCameraCutTrack())
		{
			RootTracks.Add(MakeShared<FJsonValueObject>(TrackToJson(*MovieScene, *CameraCut, bIncludeKeys)));
		}
		Json->SetArrayField(TEXT("root_tracks"), RootTracks);

		TArray<TSharedPtr<FJsonValue>> Bindings;
		for (const FMovieSceneBinding& Binding : const_cast<const UMovieScene*>(MovieScene)->GetBindings())
		{
			Bindings.Add(MakeShared<FJsonValueObject>(
				BindingToJson(*MovieScene, Binding.GetObjectGuid(), bIncludeKeys)));
		}
		Json->SetArrayField(TEXT("bindings"), Bindings);
		return Json;
	}
}
