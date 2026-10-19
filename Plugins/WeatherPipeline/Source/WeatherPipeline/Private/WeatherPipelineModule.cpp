#include "WeatherPipelineModule.h"

#include "Misc/CoreDelegates.h"

#if WITH_CESIUM_RUNTIME
#include "CesiumIonServer.h"
#include "UObject/Package.h"
#endif

IMPLEMENT_MODULE(FWeatherPipelineModule, WeatherPipeline)

const TCHAR* FWeatherPipelineModule::IonTokenVariable = TEXT("WP_CESIUM_ION_TOKEN");

namespace
{
#if WITH_CESIUM_RUNTIME
	// The asset Cesium keeps its default ion server and project token in. Its token is only what the
	// editor last SAVED: choosing a token in the Cesium panel changes it in the open editor's memory,
	// so a separate capture session would load without it.
	const TCHAR* IonServerPackage = TEXT("/Game/CesiumSettings/CesiumIonServers/CesiumIonSaaS");

	/** The default ion server - the project's asset when it exists, else an unsaved one in its place,
	 *  where Cesium looks for it. Never Cesium's own fallback: in the editor build that one saves a new
	 *  asset, which a game session must not do. */
	UCesiumIonServer* DefaultIonServer()
	{
		UPackage* Package = CreatePackage(IonServerPackage);
		Package->FullyLoad();
		if (UCesiumIonServer* Server = Cast<UCesiumIonServer>(Package->FindAssetInPackage()))
		{
			return Server;
		}
		// The class defaults are ion.cesium.com's addresses.
		return NewObject<UCesiumIonServer>(Package, TEXT("CesiumIonSaaS"), RF_Public | RF_Standalone);
	}
#endif
}

bool FWeatherPipelineModule::UseIonToken(const FString& Token)
{
#if WITH_CESIUM_RUNTIME
	if (Token.IsEmpty())
	{
		return false;
	}
	UCesiumIonServer* Server = DefaultIonServer();
	Server->DefaultIonAccessToken = Token;
	return true;
#else
	return false;
#endif
}

void FWeatherPipelineModule::StartupModule()
{
	// Before the level loads, so the tileset's first request already carries the token. Only in a game
	// session (the capture): in the editor the token stays whatever the Cesium panel set.
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([]()
	{
		const FString Token = FPlatformMisc::GetEnvironmentVariable(IonTokenVariable);
		if (GIsEditor || Token.IsEmpty())
		{
			return;
		}
		if (UseIonToken(Token))
		{
			UE_LOG(LogTemp, Log, TEXT("[Cesium] Using the Cesium ion access token from weather_pipeline.py."));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Cesium] A Cesium ion token was given, but this build has no Cesium."));
		}
	});
}

void FWeatherPipelineModule::ShutdownModule()
{
	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
}
