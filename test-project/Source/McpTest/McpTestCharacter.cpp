#include "McpTestCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "McpTestHudWidget.h"
#include "AbilitySystemComponent.h"
#include "McpTestAttributeSet.h"

AMcpTestCharacter::AMcpTestCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = true;
	Movement->RotationRate = FRotator(0.0f, 500.0f, 0.0f);
	Movement->JumpZVelocity = 500.0f;
	Movement->AirControl = 0.35f;
	Movement->MaxWalkSpeed = 500.0f;

	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(RootComponent);
	Body->SetRelativeScale3D(FVector(0.9f, 0.9f, 1.8f));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// ConstructorHelpers is the sanctioned way to reference an asset at
	// construction time; a bare LoadObject here can run during serialization.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		Body->SetStaticMesh(CubeMesh.Object);
	}

	USpringArmComponent* Boom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	Boom->SetupAttachment(RootComponent);
	Boom->TargetArmLength = 400.0f;
	Boom->bUsePawnControlRotation = true;

	UCameraComponent* Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	Camera->SetupAttachment(Boom, USpringArmComponent::SocketName);

	AbilitySystem = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystem"));
	// A default-subobject attribute set is picked up by the ASC automatically.
	Attributes = CreateDefaultSubobject<UMcpTestAttributeSet>(TEXT("Attributes"));
}

UAbilitySystemComponent* AMcpTestCharacter::GetAbilitySystemComponent() const
{
	return AbilitySystem;
}

void AMcpTestCharacter::BeginPlay()
{
	Super::BeginPlay();
	LastLocation = GetActorLocation();
	if (AbilitySystem != nullptr)
	{
		AbilitySystem->InitAbilityActorInfo(this, this);
	}
	EnsureInputAssets();

	SetupForLocalPlayer();
}

void AMcpTestCharacter::SetupForLocalPlayer()
{
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC == nullptr || !PC->IsLocalController())
	{
		return;
	}
	EnsureInputAssets();
	if (!bMappingContextAdded)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(MappingContext, 0);
			bMappingContextAdded = true;
		}
	}
	CreateHud();
}

void AMcpTestCharacter::CreateHud()
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC == nullptr || Hud != nullptr)
	{
		return;
	}
	Hud = CreateWidget<UMcpTestHudWidget>(PC, UMcpTestHudWidget::StaticClass(), TEXT("McpTestHud"));
	if (Hud == nullptr || Hud->WidgetTree == nullptr)
	{
		return;
	}
	HudText = Hud->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("HudText"));
	HudText->SetText(FText::FromString(TEXT("McpTest HUD | Distance 0 | Jumps 0")));
	Hud->WidgetTree->RootWidget = HudText;
	Hud->AddToViewport();
	UE_LOG(LogTemp, Display, TEXT("McpTest: HUD created for %s"), *PC->GetName());
}

void AMcpTestCharacter::EnsureInputAssets()
{
	if (MoveAction != nullptr)
	{
		return;
	}

	// Build Input Actions + a Mapping Context in code (no content assets).
	MoveAction = NewObject<UInputAction>(this, TEXT("IA_Move"));
	MoveAction->ValueType = EInputActionValueType::Axis2D;
	LookAction = NewObject<UInputAction>(this, TEXT("IA_Look"));
	LookAction->ValueType = EInputActionValueType::Axis2D;
	JumpAction = NewObject<UInputAction>(this, TEXT("IA_Jump"));
	JumpAction->ValueType = EInputActionValueType::Boolean;

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_McpTest"));

	// WASD -> Axis2D(X = right, Y = forward), the standard template layout.
	MappingContext->MapKey(MoveAction, EKeys::D);
	{
		FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(MoveAction, EKeys::A);
		Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(this));
	}
	{
		FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(MoveAction, EKeys::W);
		Mapping.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(this));
	}
	{
		FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(MoveAction, EKeys::S);
		Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(this));
		Mapping.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(this));
	}
	// Left stick drives the same action.
	MappingContext->MapKey(MoveAction, EKeys::Gamepad_Left2D);

	MappingContext->MapKey(LookAction, EKeys::Mouse2D);
	MappingContext->MapKey(JumpAction, EKeys::SpaceBar);
	MappingContext->MapKey(JumpAction, EKeys::Gamepad_FaceButton_Bottom);
}

void AMcpTestCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const FVector Location = GetActorLocation();
	DistanceTravelled += FVector::Dist(Location, LastLocation);
	LastLocation = Location;

	if (HudText != nullptr)
	{
		HudText->SetText(FText::FromString(FString::Printf(
			TEXT("McpTest HUD | Distance %.0f | Jumps %d"), DistanceTravelled, JumpInputCount)));
	}
}

void AMcpTestCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (Input == nullptr)
	{
		return;
	}
	// Input can be set up before BeginPlay runs, so make sure the actions exist.
	EnsureInputAssets();
	Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AMcpTestCharacter::OnMove);
	Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &AMcpTestCharacter::OnLook);
	Input->BindAction(JumpAction, ETriggerEvent::Started, this, &AMcpTestCharacter::OnJumpStarted);
	Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

	// Possession has happened by now, so the local-player setup can run even
	// if BeginPlay saw no controller.
	SetupForLocalPlayer();
}

void AMcpTestCharacter::OnMove(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	LastMoveInput = Axis;
	++MoveInputCount;

	const FRotator YawOnly(0.0f, GetControlRotation().Yaw, 0.0f);
	const FVector Forward = FRotationMatrix(YawOnly).GetUnitAxis(EAxis::X);
	const FVector Right = FRotationMatrix(YawOnly).GetUnitAxis(EAxis::Y);
	AddMovementInput(Forward, Axis.Y);
	AddMovementInput(Right, Axis.X);
}

void AMcpTestCharacter::OnLook(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	LastLookInput = Axis;
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

void AMcpTestCharacter::OnJumpStarted(const FInputActionValue& /*Value*/)
{
	++JumpInputCount;
	Jump();
}
