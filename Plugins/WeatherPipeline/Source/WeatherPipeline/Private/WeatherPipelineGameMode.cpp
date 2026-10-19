#include "WeatherPipelineGameMode.h"

#include "MyActor_weather.h"
#include "MyCharacterBase.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"

AWeatherPipelineGameMode::AWeatherPipelineGameMode()
{
	WeatherPawnClass = TSoftClassPtr<APawn>(FSoftClassPath(
		TEXT("/WeatherPipeline/Engel_unrealWeather/Blueprint/BP_engel_Character.BP_engel_Character_C")));
}

UClass* AWeatherPipelineGameMode::GetDefaultPawnClassForController_Implementation(AController* InController)
{
	if (UClass* Pawn = WeatherPawnClass.LoadSynchronous())
	{
		return Pawn;
	}

	// Fall back to the legacy /Game location if content has not been migrated yet.
	if (UClass* Pawn = LoadObject<UClass>(nullptr,
		TEXT("/Game/Engel_unrealWeather/Blueprint/BP_engel_Character.BP_engel_Character_C")))
	{
		return Pawn;
	}

	UE_LOG(LogTemp, Warning, TEXT("WeatherPipelineGameMode: BP_engel_Character not found - using default pawn."));
	return Super::GetDefaultPawnClassForController_Implementation(InController);
}

void AWeatherPipelineGameMode::StartPlay()
{
	Super::StartPlay();

	// Simulate In Editor never logs a player in, so the normal player-login path that spawns and
	// possesses the pawn does not run — yet the capture camera lives on that pawn
	// (BP_engel_Character), and Ultra Dynamic Weather centres its particles on it. Spawn it
	// explicitly when neither a player character nor a weather character already exists. In normal
	// PIE the player pawn exists, so this is skipped.
	if (!UGameplayStatics::GetPlayerCharacter(this, 0) &&
		!UGameplayStatics::GetActorOfClass(GetWorld(), AMyCharacterBase::StaticClass()))
	{
		if (UClass* PawnClass = GetDefaultPawnClassForController_Implementation(nullptr))
		{
			// Place at a PlayerStart if the level has one, otherwise at the world origin.
			FTransform SpawnTM = FTransform::Identity;
			if (AActor* Start = FindPlayerStart(nullptr))
			{
				SpawnTM = Start->GetActorTransform();
			}

			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (APawn* WeatherPawn = GetWorld()->SpawnActor<APawn>(PawnClass, SpawnTM, Params))
			{
				UE_LOG(LogTemp, Log, TEXT("WeatherPipelineGameMode: spawned weather pawn '%s' for Simulate In Editor (no player possession)."), *WeatherPawn->GetName());
			}
		}
	}

	if (bAutoSpawnWeatherActor &&
		!UGameplayStatics::GetActorOfClass(GetWorld(), AMyActor_weather::StaticClass()))
	{
		GetWorld()->SpawnActor<AMyActor_weather>();
		UE_LOG(LogTemp, Log, TEXT("WeatherPipelineGameMode: spawned AMyActor_weather automatically."));
	}
}
