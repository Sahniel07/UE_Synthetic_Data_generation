// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MyActor_controllingWeather.generated.h"

class AMyActor_weather; // Forward declare
//class ACharacter; // Forward declare ACharacter
//class UNiagaraComponent; // Forward declare NiagaraComponent
UCLASS()
class WEATHERPIPELINE_API AMyActor_controllingWeather : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AMyActor_controllingWeather();
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Weather")
	AMyActor_weather* WeatherManager; // Store reference



protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

private:

	// Initialize rain spawn rate
	float rainRate = 1.0f ; // cm/s
	float snowRate = 1.0f ;   // cm/s
	float timediff = 0.0f ;
	float frameRate = 10.0f;
	bool WaitingForDestination = false; // Flag to check if waiting for destination

};
