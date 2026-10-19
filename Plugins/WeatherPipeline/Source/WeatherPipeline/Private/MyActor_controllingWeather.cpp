// Fill out your copyright notice in the Description page of Project Settings.


#include "MyActor_controllingWeather.h"
#include "MyActor_weather.h"

#include "GameFramework/Character.h" // For ACharacter
#include "Components/CapsuleComponent.h" // For UCapsuleComponent
#include "NiagaraComponent.h" // For UNiagaraComponent
#include "Kismet/GameplayStatics.h" // For UGameplayStatics::GetPlayerCharacter

// Sets default values
AMyActor_controllingWeather::AMyActor_controllingWeather()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

}

// Called when the game starts or when spawned
void AMyActor_controllingWeather::BeginPlay()
{
	Super::BeginPlay();

    // Find the weather actor in the world
    TArray<AActor*> FoundActors;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMyActor_weather::StaticClass(), FoundActors);

    if (FoundActors.Num() > 0)
    {
        WeatherManager = Cast<AMyActor_weather>(FoundActors[0]);
        if (!WeatherManager)
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to cast found actor to AMyActor_Weather!"));
        }
        UE_LOG(LogTemp, Log, TEXT("Cast found actor to AMyActor_Weather!"));
    }
    else
    {
        //UE_WARNING_LOG(LogTemp, TEXT("No AMyActor_Weather found in the level!"));
        UE_LOG(LogTemp, Error, TEXT("No AMyActor_Weather found in the level!"));
    }

    // Example: Activate rain after 5 seconds in BeginPlay
     //GetWorldTimerManager().SetTimer(RainTimerHandle, this, &AMyActor_controllingWeather::ToggleRain, 5.0f, false);

    // Example: Start a timer to toggle rain every 10 seconds after 5 seconds initial delay
    if (WeatherManager) // Only set timer if we found the weather manager
    {
        WeatherManager->InitializeCharacterReference();
        //WeatherManager->StartRain();
        //SetRainRate(rainSpawnRate);
        //FVector ForwardDirection = PlayerCharacter->GetActorForwardVector();
        //MoveObject(FVector::ForwardVector, 1.0f);
        //WeatherManager->CameraDirection = FVector::ForwardVector;
        //WeatherManager->SpeedScale = 0.01f;
        //WeatherManager->MoveObject();
        //GetWorldTimerManager().SetTimer(MoveToggleTimerHandle, this, &AMyActor_controllingWeather::MoveObject, 10.0f, true, 5.0f);
		//WeatherManager->StartRain(rainRate); // Start rain with precipitation rate
        //WeatherManager->StartCapture( frameRate);
        //WeatherManager->StartStreamRTSP();
        //WeatherManager->MoveForwardForDistance(400.0f, 20.0f);
		//WeatherManager->StartSnow(snowRate);
		//WeatherManager->StopRain(); // Stop rain
		//WeatherManager->StopCapture();
        //GetWorldTimerManager().SetTimer(RainToggleTimerHandle, this, &AMyActor_controllingWeather::ToggleRain, 10.0f, true, 5.0f);
        
    }
	
}

// Called every frame
void AMyActor_controllingWeather::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

    //timediff += DeltaTime;
    //if (timediff >= 50 && timediff < 50 + DeltaTime) {
    //    WeatherManager->StartSnow(snowRate);
    //    WeatherManager->StopRain();
    //    //WeatherManager->MoveForwardForDistance(400.0f, 20.0f);
    //    //FRotator ang = FRotator(0.0f, 90.0f, 0.0f);
    //    //WeatherManager->RotateCCW(ang, 10.f);
    //}



}




