// Weather pipeline character base class.
// BP_engel_Character is parented to this class. It carries the weather toggle flags
// polled by AMyActor_weather::Tick and binds the P key to start the capture pipeline.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "MyCharacterBase.generated.h"

class UInputAction;
class UInputMappingContext;

UCLASS()
class WEATHERPIPELINE_API AMyCharacterBase : public ACharacter
{
	GENERATED_BODY()

public:
	AMyCharacterBase();

	UPROPERTY(BlueprintReadOnly, Category = "Camera")
	FVector2D CamRot;

	UPROPERTY(BlueprintReadOnly, Category = "Camera")
	bool scriptRot = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool recImg_resize = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool recImg = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool rainVar = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool snowVar = false;

	// Blueprint toggle: flipping this triggers StartFog / StopFog (manual override of auto-chain)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool fogVar = false;

	// Blueprint-only toggle: triggers RunWeatherLitmusTest() (visual sanity check), then
	// auto-chains to the full pipeline. No longer bound to the P key — use pipelineVar for that.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool litmusVar = false;

	// Set by the P key via OnStartPipelinePressed. Triggers the real Rain->Snow->Fog
	// capture run directly, skipping the litmus preview, so PIE captures images and exits.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool pipelineVar = false;

	// Input action that starts the weather pipeline (default: plugin's IA_StartWeatherPipeline, mapped to P).
	// Soft reference so CDO construction never depends on plugin content mount order.
	UPROPERTY(EditDefaultsOnly, Category = "Weather|Input")
	TSoftObjectPtr<UInputAction> StartPipelineAction;

	// Mapping context containing the pipeline key mapping (default: plugin's IMC_Default).
	UPROPERTY(EditDefaultsOnly, Category = "Weather|Input")
	TSoftObjectPtr<UInputMappingContext> WeatherMappingContext;

	// Drone/camera flight speed (cm/s). Applied to the CharacterMovementComponent at BeginPlay
	// for both flying and walking modes, so manual navigation of the capture camera is faster.
	// Tune in the BP_engel_Character defaults or here. Default engine fly speed is 600.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Movement")
	float CameraMoveSpeed = 12000.0f;

	void ApplyVirtualLook(const FVector2D& camRot);

protected:
	virtual void BeginPlay() override;

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	// P-key handler — feeds the existing polled-bool path in AMyActor_weather::Tick,
	// so the Details-panel litmusVar toggle keeps working as a fallback.
	void OnStartPipelinePressed();
};
