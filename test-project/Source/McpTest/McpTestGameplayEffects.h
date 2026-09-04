// Native gameplay effects and an ability for gas_ops verification: an instant
// -10 Health hit, a 5-second +100 Speed boost, and a no-op ability.
#pragma once

#include "Abilities/GameplayAbility.h"
#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "McpTestGameplayEffects.generated.h"

UCLASS()
class UMcpTestDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UMcpTestDamageEffect();
};

UCLASS()
class UMcpTestSpeedBoostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UMcpTestSpeedBoostEffect();
};

UCLASS()
class UMcpTestPingAbility : public UGameplayAbility
{
	GENERATED_BODY()
public:
	UMcpTestPingAbility();
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
