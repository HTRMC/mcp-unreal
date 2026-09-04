#include "McpTestGameplayEffects.h"

#include "McpTestAttributeSet.h"

namespace
{
	FGameplayModifierInfo AdditiveModifier(const FGameplayAttribute& Attribute, float Magnitude)
	{
		FGameplayModifierInfo Info;
		Info.Attribute = Attribute;
		Info.ModifierOp = EGameplayModOp::Additive;
		Info.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Magnitude));
		return Info;
	}
}

UMcpTestDamageEffect::UMcpTestDamageEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	Modifiers.Add(AdditiveModifier(UMcpTestAttributeSet::GetHealthAttribute(), -10.0f));
}

UMcpTestSpeedBoostEffect::UMcpTestSpeedBoostEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(5.0f));
	Modifiers.Add(AdditiveModifier(UMcpTestAttributeSet::GetSpeedAttribute(), 100.0f));
}

UMcpTestPingAbility::UMcpTestPingAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
}

void UMcpTestPingAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("McpTest: ping ability activated on %s"),
		ActorInfo && ActorInfo->AvatarActor.IsValid() ? *ActorInfo->AvatarActor->GetName() : TEXT("?"));
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
