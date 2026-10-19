#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FWeatherPipelineModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Environment variable weather_pipeline.py hands the checked Cesium ion access token in. */
	static const TCHAR* IonTokenVariable;

	/**
	 * Makes Token the project's default Cesium ion access token for this session, in memory only:
	 * every tileset that has no token of its own loads with it. Nothing is saved - the token never
	 * reaches Content/ or the ini. False when Cesium is not part of the build or Token is empty.
	 */
	static bool UseIonToken(const FString& Token);

private:
	FDelegateHandle PostEngineInitHandle;
};
