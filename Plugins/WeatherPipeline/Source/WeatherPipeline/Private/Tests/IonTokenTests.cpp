#include "Misc/AutomationTest.h"
#include "WeatherPipelineModule.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_CESIUM_RUNTIME

#include "CesiumIonServer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineIonTokenTest,
	"WeatherPipeline.Cesium.TokenFromThePipelineReachesTheTileset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineIonTokenTest::RunTest(const FString& Parameters)
{
	// The capture session gets its token from weather_pipeline.py, not from what the editor saved: it
	// must land on the server a tileset without a token of its own reads (Cesium's default server).
	UCesiumIonServer* Server = UCesiumIonServer::GetDefaultServer();
	if (!TestNotNull(TEXT("Cesium's default ion server"), Server)) { return false; }
	const FString Saved     = Server->DefaultIonAccessToken;
	const bool    bWasDirty = Server->GetPackage()->IsDirty();

	TestTrue(TEXT("a token is taken"), FWeatherPipelineModule::UseIonToken(TEXT("test-token")));
	TestEqual(TEXT("the default server carries it"), Server->DefaultIonAccessToken, FString(TEXT("test-token")));
	TestFalse(TEXT("an empty token is refused"), FWeatherPipelineModule::UseIonToken(FString()));
	TestEqual(TEXT("and changes nothing"), Server->DefaultIonAccessToken, FString(TEXT("test-token")));
	TestEqual(TEXT("only in memory: the asset is not marked for saving"), Server->GetPackage()->IsDirty(), bWasDirty);

	Server->DefaultIonAccessToken = Saved;   // the editor's own token back
	return true;
}

#endif
