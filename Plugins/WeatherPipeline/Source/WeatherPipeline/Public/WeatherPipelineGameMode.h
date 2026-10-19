// Drop-in game mode for the weather pipeline.
// Set this as the map's GameMode Override (or project default) and the BP_engel
// character spawns as the pawn and an AMyActor_weather is auto-created — so the
// pipeline runs in ANY map with no other setup. Press P in PIE to start it.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "WeatherPipelineGameMode.generated.h"

UCLASS()
class WEATHERPIPELINE_API AWeatherPipelineGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AWeatherPipelineGameMode();

	// Pawn used for weather capture. Defaults to the plugin's BP_engel_Character.
	// Soft reference, resolved lazily — avoids plugin content mount-order issues.
	UPROPERTY(EditDefaultsOnly, Category = "Weather")
	TSoftClassPtr<APawn> WeatherPawnClass;

	// Spawn an AMyActor_weather automatically if the map has none.
	UPROPERTY(EditDefaultsOnly, Category = "Weather")
	bool bAutoSpawnWeatherActor = true;

	virtual UClass* GetDefaultPawnClassForController_Implementation(AController* InController) override;
	virtual void StartPlay() override;
};
