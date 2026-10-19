#include "MyCharacterBase.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"

AMyCharacterBase::AMyCharacterBase()
{
	// Defaults point at plugin content. Both load lazily in SetupPlayerInputComponent,
	// so a missing asset degrades to "no P key" rather than a load-order failure.
	StartPipelineAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(
		TEXT("/WeatherPipeline/Engel_unrealWeather/Input/Actions/IA_StartWeatherPipeline.IA_StartWeatherPipeline")));
	WeatherMappingContext = TSoftObjectPtr<UInputMappingContext>(FSoftObjectPath(
		TEXT("/WeatherPipeline/Engel_unrealWeather/Input/IMC_Default.IMC_Default")));
}

void AMyCharacterBase::BeginPlay()
{
	Super::BeginPlay();

	// Speed up manual navigation of the capture drone. Applies to both flying and walking
	// movement modes plus acceleration, so the camera reaches and reframes shots quickly.
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->MaxFlySpeed      = CameraMoveSpeed;
		Move->MaxWalkSpeed     = CameraMoveSpeed;
		Move->MaxAcceleration  = CameraMoveSpeed * 4.0f;
		Move->BrakingDecelerationFlying  = CameraMoveSpeed * 4.0f;
		Move->BrakingDecelerationWalking = CameraMoveSpeed * 4.0f;
	}
}

void AMyCharacterBase::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (const APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsys =
				ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			if (UInputMappingContext* IMC = WeatherMappingContext.LoadSynchronous())
			{
				// Idempotent vs. the Blueprint's own AddMappingContext on BeginPlay
				if (!Subsys->HasMappingContext(IMC))
				{
					Subsys->AddMappingContext(IMC, 0);
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("AMyCharacterBase: WeatherMappingContext failed to load (%s)."),
					*WeatherMappingContext.ToString());
			}
		}
	}

	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		if (UInputAction* IA = StartPipelineAction.LoadSynchronous())
		{
			EIC->BindAction(IA, ETriggerEvent::Started, this, &AMyCharacterBase::OnStartPipelinePressed);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("AMyCharacterBase: StartPipelineAction failed to load (%s). P key will not trigger the pipeline."),
				*StartPipelineAction.ToString());
		}
	}
}

void AMyCharacterBase::OnStartPipelinePressed()
{
	UE_LOG(LogTemp, Log, TEXT("Weather pipeline trigger pressed (P)."));
	pipelineVar = true; // picked up by AMyActor_weather::Tick next frame
}

void AMyCharacterBase::ApplyVirtualLook(const FVector2D& camRot)
{
	AddControllerYawInput(camRot.X);
	AddControllerPitchInput(camRot.Y);
}
