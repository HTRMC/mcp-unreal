// Gameplay Ability System interop: read an actor's ability system state and
// drive it — grant/activate abilities, apply/remove effects, set attribute
// base values, add/remove loose tags.
//
// Everything goes through UAbilitySystemComponent's own API on the game
// thread, so what an agent does here is exactly what gameplay code can do.

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Abilities/GameplayAbility.h"
#include "AttributeSet.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace
	{
		UAbilitySystemComponent* AscOrError(
			const TSharedRef<FJsonObject>& Body, const TCHAR* Field, const TSharedRef<FMcpResponder>& Responder)
		{
			FString ActorSpec;
			Body->TryGetStringField(Field, ActorSpec);
			if (ActorSpec.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					FString::Printf(TEXT("'%s' (actor path or label) is required"), Field));
				return nullptr;
			}
			UWorld* World = ResolveWorld(Body);
			if (World == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
					TEXT("no matching world — is PIE running?"));
				return nullptr;
			}
			AActor* Actor = ResolveActor(World, ActorSpec);
			if (Actor == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
					FString::Printf(TEXT("no actor '%s' in %s"), *ActorSpec, *World->GetName()));
				return nullptr;
			}
			UAbilitySystemComponent* Asc =
				UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Actor, /*LookForComponent*/ true);
			if (Asc == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("no_ability_system"),
					FString::Printf(TEXT("'%s' has no AbilitySystemComponent"), *Actor->GetName()));
			}
			return Asc;
		}

		template <typename T>
		UClass* SubclassOrError(
			const TSharedRef<FJsonObject>& Body, const TCHAR* Field, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			Body->TryGetStringField(Field, Spec);
			if (Spec.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					FString::Printf(TEXT("'%s' class is required, e.g. GA_Jump or /Game/Abilities/GA_Jump"), Field));
				return nullptr;
			}
			UClass* Class = ResolveClass(Spec);
			if (Class == nullptr || !Class->IsChildOf(T::StaticClass()))
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("class_not_found"),
					FString::Printf(TEXT("'%s' is not a %s class — see gas_ops list_classes"),
						*Spec, *T::StaticClass()->GetName()));
				return nullptr;
			}
			return Class;
		}

		TArray<TSharedPtr<FJsonValue>> TagsToJson(const FGameplayTagContainer& Tags)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FGameplayTag& Tag : Tags)
			{
				Out.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			return Out;
		}

		/// Find an attribute by "Health" or "McpTestAttributeSet.Health".
		bool FindAttribute(UAbilitySystemComponent* Asc, const FString& Spec, FGameplayAttribute& Out, TArray<FString>& Names)
		{
			FString SetName, AttrName = Spec;
			Spec.Split(TEXT("."), &SetName, &AttrName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			for (UAttributeSet* Set : Asc->GetSpawnedAttributes())
			{
				if (Set == nullptr)
				{
					continue;
				}
				TArray<FGameplayAttribute> Attributes;
				UAttributeSet::GetAttributesFromSetClass(Set->GetClass(), Attributes);
				for (const FGameplayAttribute& Attribute : Attributes)
				{
					const FString Full = Set->GetClass()->GetName() + TEXT(".") + Attribute.GetName();
					Names.Add(Full);
					if (Attribute.GetName().Equals(AttrName, ESearchCase::IgnoreCase)
						&& (SetName.IsEmpty() || Set->GetClass()->GetName().Equals(SetName, ESearchCase::IgnoreCase)))
					{
						Out = Attribute;
						return true;
					}
				}
			}
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> AttributesToJson(UAbilitySystemComponent* Asc)
		{
			TArray<TSharedPtr<FJsonValue>> Sets;
			for (UAttributeSet* Set : Asc->GetSpawnedAttributes())
			{
				if (Set == nullptr)
				{
					continue;
				}
				const TSharedRef<FJsonObject> SetJson = MakeShared<FJsonObject>();
				SetJson->SetStringField(TEXT("class"), Set->GetClass()->GetName());
				TArray<FGameplayAttribute> Attributes;
				UAttributeSet::GetAttributesFromSetClass(Set->GetClass(), Attributes);
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FGameplayAttribute& Attribute : Attributes)
				{
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), Attribute.GetName());
					Item->SetNumberField(TEXT("current"), Asc->GetNumericAttribute(Attribute));
					Item->SetNumberField(TEXT("base"), Asc->GetNumericAttributeBase(Attribute));
					Values.Add(MakeShared<FJsonValueObject>(Item));
				}
				SetJson->SetArrayField(TEXT("attributes"), Values);
				Sets.Add(MakeShared<FJsonValueObject>(SetJson));
			}
			return Sets;
		}

		TArray<TSharedPtr<FJsonValue>> AbilitiesToJson(UAbilitySystemComponent* Asc)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FGameplayAbilitySpec& Spec : Asc->GetActivatableAbilities())
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("handle"), Spec.Handle.ToString());
				Item->SetStringField(TEXT("class"), Spec.Ability ? Spec.Ability->GetClass()->GetPathName() : TEXT(""));
				Item->SetNumberField(TEXT("level"), Spec.Level);
				Item->SetNumberField(TEXT("input_id"), Spec.InputID);
				Item->SetBoolField(TEXT("active"), Spec.IsActive());
				if (Spec.Ability != nullptr)
				{
					Item->SetArrayField(TEXT("tags"), TagsToJson(Spec.Ability->GetAssetTags()));
				}
				Item->SetArrayField(TEXT("dynamic_tags"), TagsToJson(Spec.GetDynamicSpecSourceTags()));
				Out.Add(MakeShared<FJsonValueObject>(Item));
			}
			return Out;
		}

		TArray<TSharedPtr<FJsonValue>> EffectsToJson(UAbilitySystemComponent* Asc)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			const FActiveGameplayEffectsContainer& Container = Asc->GetActiveGameplayEffects();
			const float WorldTime = Container.GetWorldTime();
			for (auto It = Container.CreateConstIterator(); It; ++It)
			{
				const FActiveGameplayEffect& Effect = *It;
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("handle"), Effect.Handle.ToString());
				Item->SetStringField(TEXT("effect"),
					Effect.Spec.Def ? Effect.Spec.Def->GetClass()->GetPathName() : TEXT(""));
				Item->SetNumberField(TEXT("level"), Effect.Spec.GetLevel());
				Item->SetNumberField(TEXT("duration"), Effect.GetDuration());
				Item->SetNumberField(TEXT("time_remaining"), Effect.GetTimeRemaining(WorldTime));
				Item->SetNumberField(TEXT("stacks"), Effect.Spec.GetStackCount());
				FGameplayTagContainer Granted;
				Effect.Spec.GetAllGrantedTags(Granted);
				Item->SetArrayField(TEXT("granted_tags"), TagsToJson(Granted));
				Out.Add(MakeShared<FJsonValueObject>(Item));
			}
			return Out;
		}

		TSharedRef<FJsonObject> StateToJson(UAbilitySystemComponent* Asc)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("ability_system"), Asc->GetPathName());
			Data->SetStringField(TEXT("owner"), Asc->GetOwnerActor() ? Asc->GetOwnerActor()->GetPathName() : TEXT(""));
			Data->SetStringField(TEXT("avatar"), Asc->GetAvatarActor() ? Asc->GetAvatarActor()->GetPathName() : TEXT(""));
			Data->SetArrayField(TEXT("attribute_sets"), AttributesToJson(Asc));
			Data->SetArrayField(TEXT("abilities"), AbilitiesToJson(Asc));
			Data->SetArrayField(TEXT("active_effects"), EffectsToJson(Asc));
			FGameplayTagContainer Owned;
			Asc->GetOwnedGameplayTags(Owned);
			Data->SetArrayField(TEXT("owned_tags"), TagsToJson(Owned));
			return Data;
		}

		bool TagOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder, FGameplayTag& Out)
		{
			FString TagName;
			Body->TryGetStringField(TEXT("tag"), TagName);
			if (TagName.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'tag' is required, e.g. State.Stunned"));
				return false;
			}
			Out = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound*/ false);
			if (!Out.IsValid())
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("tag_not_registered"),
					FString::Printf(
						TEXT("gameplay tag '%s' is not registered — add it under [/Script/GameplayTags.GameplayTagsList] ")
						TEXT("in Config/DefaultGameplayTags.ini (config_ops) and restart, or use a tag from get_state"),
						*TagName));
				return false;
			}
			return true;
		}
	}

	void RegisterGasRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/gas/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_classes"))
				{
					FString Kind = TEXT("abilities"), Filter;
					Body->TryGetStringField(TEXT("kind"), Kind);
					Body->TryGetStringField(TEXT("filter"), Filter);
					UClass* Base = Kind == TEXT("effects") ? UGameplayEffect::StaticClass()
						: Kind == TEXT("attribute_sets") ? UAttributeSet::StaticClass()
						: UGameplayAbility::StaticClass();
					TArray<TSharedPtr<FJsonValue>> Classes;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!Class->IsChildOf(Base) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
							|| Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_")))
						{
							continue;
						}
						if (!Filter.IsEmpty() && !Class->GetName().Contains(Filter))
						{
							continue;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Class->GetName());
						Item->SetStringField(TEXT("path"), Class->GetPathName());
						Classes.Add(MakeShared<FJsonValueObject>(Item));
						if (Classes.Num() >= 300)
						{
							break;
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("kind"), Kind);
					Data->SetStringField(TEXT("note"),
						TEXT("only loaded classes are listed; Blueprint classes appear once their asset is loaded (search_assets + get_asset_info loads it)"));
					Data->SetArrayField(TEXT("classes"), Classes);
					Responder->Ok(Data);
					return;
				}

				UAbilitySystemComponent* Asc = AscOrError(Body, TEXT("actor"), Responder);
				if (!Asc) { return; }

				if (Operation == TEXT("get_state"))
				{
					Responder->Ok(StateToJson(Asc));
					return;
				}

				if (Operation == TEXT("give_ability"))
				{
					UClass* Class = SubclassOrError<UGameplayAbility>(Body, TEXT("ability"), Responder);
					if (!Class) { return; }
					double Level = 1, InputId = INDEX_NONE;
					Body->TryGetNumberField(TEXT("level"), Level);
					Body->TryGetNumberField(TEXT("input_id"), InputId);
					const FGameplayAbilitySpecHandle Handle = Asc->GiveAbility(
						FGameplayAbilitySpec(Class, static_cast<int32>(Level), static_cast<int32>(InputId), Asc->GetOwner()));
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("handle"), Handle.ToString());
					Data->SetBoolField(TEXT("granted"), Handle.IsValid());
					Data->SetArrayField(TEXT("abilities"), AbilitiesToJson(Asc));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_ability"))
				{
					FString HandleText;
					Body->TryGetStringField(TEXT("handle"), HandleText);
					FGameplayAbilitySpecHandle Handle;
					if (!HandleText.IsEmpty())
					{
						for (const FGameplayAbilitySpec& Spec : Asc->GetActivatableAbilities())
						{
							if (Spec.Handle.ToString() == HandleText)
							{
								Handle = Spec.Handle;
							}
						}
					}
					else
					{
						UClass* Class = SubclassOrError<UGameplayAbility>(Body, TEXT("ability"), Responder);
						if (!Class) { return; }
						if (const FGameplayAbilitySpec* Spec = Asc->FindAbilitySpecFromClass(Class))
						{
							Handle = Spec->Handle;
						}
					}
					if (!Handle.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("ability_not_granted"),
							TEXT("no granted ability matches — see get_state abilities"));
						return;
					}
					Asc->ClearAbility(Handle);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Handle.ToString());
					Data->SetArrayField(TEXT("abilities"), AbilitiesToJson(Asc));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("activate_ability"))
				{
					bool bActivated = false;
					FString TagName;
					if (Body->TryGetStringField(TEXT("tag"), TagName) && !TagName.IsEmpty())
					{
						FGameplayTag Tag;
						if (!TagOrError(Body, Responder, Tag)) { return; }
						bActivated = Asc->TryActivateAbilitiesByTag(FGameplayTagContainer(Tag));
					}
					else
					{
						UClass* Class = SubclassOrError<UGameplayAbility>(Body, TEXT("ability"), Responder);
						if (!Class) { return; }
						if (Asc->FindAbilitySpecFromClass(Class) == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("ability_not_granted"),
								FString::Printf(TEXT("'%s' has not been granted — give_ability first"), *Class->GetName()));
							return;
						}
						bActivated = Asc->TryActivateAbilityByClass(Class);
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("activated"), bActivated);
					Data->SetArrayField(TEXT("abilities"), AbilitiesToJson(Asc));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("cancel_abilities"))
				{
					Asc->CancelAllAbilities();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("abilities"), AbilitiesToJson(Asc));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("apply_effect"))
				{
					UClass* Class = SubclassOrError<UGameplayEffect>(Body, TEXT("effect"), Responder);
					if (!Class) { return; }
					double Level = 1;
					Body->TryGetNumberField(TEXT("level"), Level);
					FActiveGameplayEffectHandle Handle;
					FString SourceSpec;
					if (Body->TryGetStringField(TEXT("source"), SourceSpec) && !SourceSpec.IsEmpty())
					{
						UAbilitySystemComponent* Source = AscOrError(Body, TEXT("source"), Responder);
						if (!Source) { return; }
						const FGameplayEffectSpecHandle Spec =
							Source->MakeOutgoingSpec(Class, static_cast<float>(Level), Source->MakeEffectContext());
						if (Spec.IsValid())
						{
							Handle = Source->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Asc);
						}
					}
					else
					{
						Handle = Asc->ApplyGameplayEffectToSelf(
							Class->GetDefaultObject<UGameplayEffect>(), static_cast<float>(Level), Asc->MakeEffectContext());
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					// Instant effects apply and return an invalid handle by design.
					Data->SetStringField(TEXT("handle"), Handle.ToString());
					Data->SetBoolField(TEXT("has_active_handle"), Handle.IsValid());
					Data->SetArrayField(TEXT("attribute_sets"), AttributesToJson(Asc));
					Data->SetArrayField(TEXT("active_effects"), EffectsToJson(Asc));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_effect"))
				{
					FString HandleText;
					Body->TryGetStringField(TEXT("handle"), HandleText);
					int32 Removed = 0;
					if (!HandleText.IsEmpty())
					{
						TArray<FActiveGameplayEffectHandle> Matches;
						for (auto It = Asc->GetActiveGameplayEffects().CreateConstIterator(); It; ++It)
						{
							if ((*It).Handle.ToString() == HandleText)
							{
								Matches.Add((*It).Handle);
							}
						}
						for (const FActiveGameplayEffectHandle& Handle : Matches)
						{
							Removed += Asc->RemoveActiveGameplayEffect(Handle) ? 1 : 0;
						}
					}
					else
					{
						UClass* Class = SubclassOrError<UGameplayEffect>(Body, TEXT("effect"), Responder);
						if (!Class) { return; }
						const int32 Before = Asc->GetActiveGameplayEffects().GetNumGameplayEffects();
						Asc->RemoveActiveGameplayEffectBySourceEffect(Class, nullptr);
						Removed = Before - Asc->GetActiveGameplayEffects().GetNumGameplayEffects();
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("removed"), Removed);
					Data->SetArrayField(TEXT("active_effects"), EffectsToJson(Asc));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_attribute"))
				{
					FString AttributeSpec;
					double Value = 0.0;
					Body->TryGetStringField(TEXT("attribute"), AttributeSpec);
					if (AttributeSpec.IsEmpty() || !Body->TryGetNumberField(TEXT("value"), Value))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'attribute' (e.g. Health or MySet.Health) and numeric 'value' are required"));
						return;
					}
					FGameplayAttribute Attribute;
					TArray<FString> Names;
					if (!FindAttribute(Asc, AttributeSpec, Attribute, Names))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("attribute_not_found"),
							FString::Printf(TEXT("no attribute '%s' — available: %s"), *AttributeSpec,
								Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", "))));
						return;
					}
					Asc->SetNumericAttributeBase(Attribute, static_cast<float>(Value));
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("attribute"), Attribute.GetName());
					Data->SetNumberField(TEXT("base"), Asc->GetNumericAttributeBase(Attribute));
					Data->SetNumberField(TEXT("current"), Asc->GetNumericAttribute(Attribute));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_tag") || Operation == TEXT("remove_tag"))
				{
					FGameplayTag Tag;
					if (!TagOrError(Body, Responder, Tag)) { return; }
					double Count = 1;
					Body->TryGetNumberField(TEXT("count"), Count);
					if (Operation == TEXT("add_tag"))
					{
						Asc->AddLooseGameplayTag(Tag, static_cast<int32>(Count));
					}
					else
					{
						Asc->RemoveLooseGameplayTag(Tag, static_cast<int32>(Count));
					}
					FGameplayTagContainer Owned;
					Asc->GetOwnedGameplayTags(Owned);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("tag"), Tag.ToString());
					Data->SetArrayField(TEXT("owned_tags"), TagsToJson(Owned));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_classes, get_state, give_ability, remove_ability, ")
						TEXT("activate_ability, cancel_abilities, apply_effect, remove_effect, set_attribute, add_tag, or remove_tag"),
						*Operation));
			});
	}
}
