// A minimal Enhanced Input character used to verify McpLink input injection.
// Its Input Actions and Mapping Context are built in C++ at runtime, so the
// test project needs no content assets.
#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/Character.h"
#include "McpTestCharacter.generated.h"

class UAbilitySystemComponent;
class UInputAction;
class UMcpTestAttributeSet;
class UInputMappingContext;
struct FInputActionValue;

UCLASS()
class AMcpTestCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AMcpTestCharacter();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	/// Total distance travelled and input events seen — the assertions an
	/// injection test reads back through McpLink's reflection routes.
	UPROPERTY(BlueprintReadOnly, Category = "McpTest")
	float DistanceTravelled = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "McpTest")
	int32 MoveInputCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "McpTest")
	int32 JumpInputCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "McpTest")
	FVector2D LastMoveInput = FVector2D::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "McpTest")
	FVector2D LastLookInput = FVector2D::ZeroVector;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

private:
	/// Builds the Input Actions and Mapping Context once. Called from both
	/// BeginPlay and SetupPlayerInputComponent, whose order is not guaranteed.
	void EnsureInputAssets();

	void OnMove(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnJumpStarted(const FInputActionValue& Value);

	/// A plain cube body so injected movement is visible in viewport captures.
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> Body;

	/// Adds the mapping context and builds the HUD once a local player
	/// controller is attached. The default pawn's BeginPlay runs before it is
	/// possessed, so this is called from both BeginPlay and
	/// SetupPlayerInputComponent and does nothing until a controller exists.
	void SetupForLocalPlayer();
	bool bMappingContextAdded = false;

	/// A runtime-built UMG HUD (no Blueprint asset) so ui_query has real
	/// text to read: "McpTest HUD | Distance N | Jumps N".
	void CreateHud();
	UPROPERTY() TObjectPtr<class UUserWidget> Hud;
	UPROPERTY() TObjectPtr<class UTextBlock> HudText;

	/// Gameplay Ability System surface for gas_ops verification.
	UPROPERTY() TObjectPtr<UAbilitySystemComponent> AbilitySystem;
	UPROPERTY() TObjectPtr<UMcpTestAttributeSet> Attributes;

	UPROPERTY() TObjectPtr<UInputMappingContext> MappingContext;
	UPROPERTY() TObjectPtr<UInputAction> MoveAction;
	UPROPERTY() TObjectPtr<UInputAction> LookAction;
	UPROPERTY() TObjectPtr<UInputAction> JumpAction;

	FVector LastLocation = FVector::ZeroVector;
};
