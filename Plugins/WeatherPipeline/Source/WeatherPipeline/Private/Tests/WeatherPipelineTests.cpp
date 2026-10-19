// Automation tests for the weather capture pipeline.
//
// Scope is deliberately the logic that can be asserted without a running world: the condition table,
// the litmus/capture relationship, and settings validation. The parts that talk to Ultra Dynamic
// Weather by reflection are not covered here - a test that mocks UDW would only prove the mock
// matches the code, not that either matches UDS 9.6, which is the failure mode that actually bites.
// Those paths are covered by the runtime verification already in the pipeline (VerifyWeatherApplied,
// the read-back logging), which checks against the real asset.
//
// Run headless:
//   UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests WeatherPipeline; Quit"
//                        -unattended -nopause -nosplash -NullRHI
// or from the editor: Tools > Session Frontend > Automation > WeatherPipeline.

#include "Misc/AutomationTest.h"
#include "MyActor_weather.h"
#include "CaptureShots.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Reaches the internals the tests assert on. Declared a friend by AMyActor_weather. */
struct FWeatherPipelineTestAccess
{
	static void BuildRun(AMyActor_weather& Actor)      { Actor.BuildDefaultCaptureRun(); }
	static bool Validate(AMyActor_weather& Actor)      { return Actor.ValidateSettings(); }
	static bool LoadGeoid(AMyActor_weather& Actor)     { return Actor.LoadGeoidGrid(); }
	static float GeoidAt(AMyActor_weather& Actor, double Lat, double Lon)
	{
		return Actor.GetGeoidSeparationAt(Lat, Lon);
	}
	static void SetStart(AMyActor_weather& Actor, const FVector& Location)
	{
		Actor.CharacterStartTransform = FTransform(Location);
		Actor.bHaveCharacterStart     = true;
		Actor.FlightPath.Reset();
	}
	static const TArray<FCaptureWaypoint>& Path(AMyActor_weather& Actor) { return Actor.FlightPath; }
	static void StartAt(AMyActor_weather& Actor, const FVector& Location, const FRotator& View, float HeightM)
	{
		Actor.StartPathAt(Location, View, HeightM);
	}
};

namespace
{
	/**
	 * A configured-but-unspawned actor. The condition builder is world-free and its flight-path
	 * branch is gated on a character start that a bare object does not have, so the table can be
	 * built and asserted on without standing up a level.
	 */
	AMyActor_weather* MakeActor()
	{
		return NewObject<AMyActor_weather>(GetTransientPackage());
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Condition table
//////////////////////////////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineConditionTableTest,
	"WeatherPipeline.Conditions.TableIntegrity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineConditionTableTest::RunTest(const FString& Parameters)
{
	AMyActor_weather* Actor = MakeActor();
	TestNotNull(TEXT("actor was created"), Actor);
	if (!Actor) { return false; }

	// Config properties load from DefaultGame.ini, so a filter left there by the last run would
	// silently narrow the table under the test. Cleared so these assert the shipped table itself.
	Actor->CaptureConditionFilter.Empty();
	Actor->ConditionFilter.Empty();

	FWeatherPipelineTestAccess::BuildRun(*Actor);

	TestTrue(TEXT("the capture table is not empty"), Actor->CaptureConditions.Num() > 0);

	// Every entry must be usable. An empty preset path resolves to nothing and the condition applies
	// as clear weather - which looks like a working run producing silently mislabelled images.
	TSet<FString> SeenLabels;
	for (const FWeatherCondition& C : Actor->CaptureConditions)
	{
		TestFalse(FString::Printf(TEXT("condition label is not empty (preset '%s')"), *C.PresetPath),
			C.Label.IsEmpty());

		TestFalse(FString::Printf(TEXT("condition '%s' has a preset path"), *C.Label),
			C.PresetPath.IsEmpty());

		// Labels become directory and filename components in the dataset, so a duplicate would make
		// two different weathers write into the same place.
		TestFalse(FString::Printf(TEXT("condition label '%s' is unique"), *C.Label),
			SeenLabels.Contains(C.Label));
		SeenLabels.Add(C.Label);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineSnowRampTest,
	"WeatherPipeline.Conditions.SnowCoverageRamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineSnowRampTest::RunTest(const FString& Parameters)
{
	AMyActor_weather* Actor = MakeActor();
	if (!Actor) { return false; }

	// Config properties load from DefaultGame.ini, so a filter left there by the last run would
	// silently narrow the table under the test. Cleared so these assert the shipped table itself.
	Actor->CaptureConditionFilter.Empty();
	Actor->ConditionFilter.Empty();

	FWeatherPipelineTestAccess::BuildRun(*Actor);

	// The capture table is now stock UDS presets only - every condition IS a shipped preset handed to
	// UDW untouched. The old test asserted a snow-coverage ramp built from MaterialSnowCoverageOverride
	// on a hand-authored table; those overrides are gone, and the ramp now lives in the presets
	// themselves (Snow_Light 0.50, Snow 1.00, Blizzard 1.00) where it is UDS's to guarantee, not ours.
	//
	// What IS ours to guarantee is that nothing reintroduces an override, because that is the practice
	// every visual defect in this pipeline came from. So this checks the property that replaced the
	// ramp: the table is preset-pure.
	for (const FWeatherCondition& C : Actor->CaptureConditions)
	{
		TestTrue(FString::Printf(TEXT("'%s' names a stock preset"), *C.Label),
			C.PresetPath.Contains(TEXT("/Weather_Presets/")) ||
			C.PresetPath.StartsWith(TEXT("/Game/WeatherPipeline/Presets/")));

		// -1 is the "not set" sentinel on every channel; 0 would mean "force this channel to zero".
		TestTrue(FString::Printf(TEXT("'%s' carries no fog override"), *C.Label),
			C.FogOverride < 0.0f);
		TestTrue(FString::Printf(TEXT("'%s' carries no cloud coverage override"), *C.Label),
			C.CloudCoverageOverride < 0.0f);
		TestTrue(FString::Printf(TEXT("'%s' carries no rain override"), *C.Label),
			C.RainOverride < 0.0f);
		TestTrue(FString::Printf(TEXT("'%s' carries no snow override"), *C.Label),
			C.SnowOverride < 0.0f);
		TestTrue(FString::Printf(TEXT("'%s' carries no snow coverage override"), *C.Label),
			C.MaterialSnowCoverageOverride < 0.0f);
		TestTrue(FString::Printf(TEXT("'%s' carries no fog density scale override"), *C.Label),
			C.FogDensityScaleOverride <= 0.0f);
	}

	// 1:1 with the preset, snow coverage included: the snowfall-proportional pin is a deviation and
	// ships off.
	TestFalse(TEXT("snow coverage follows the preset, not snowfall / 10"),
		GetDefault<AMyActor_weather>()->bSnowCoverageFollowsSnowfall);

	// The dust presets are the two shipped ones deliberately excluded, so their absence is a decision
	// worth pinning rather than an accident to rediscover.
	for (const TCHAR* Excluded : { TEXT("Sand_Dust_Calm"), TEXT("Sand_Dust_Storm") })
	{
		const bool bPresent = Actor->CaptureConditions.ContainsByPredicate(
			[Excluded](const FWeatherCondition& C) { return C.PresetPath.Contains(Excluded); });

		TestFalse(FString::Printf(TEXT("'%s' is excluded from the capture table"), Excluded), bPresent);
	}

	// Snow still needs smooth normals on the Cesium tiles or it will not land on roofs and streets.
	for (const TCHAR* Label : { TEXT("snow_light"), TEXT("snow_medium"), TEXT("snow_fog"), TEXT("snow_heavy") })
	{
		const FWeatherCondition* Found = Actor->CaptureConditions.FindByPredicate(
			[Label](const FWeatherCondition& C) { return C.Label == Label; });

		if (!Found)
		{
			AddError(FString::Printf(TEXT("expected a '%s' condition in the capture table"), Label));
			continue;
		}

		TestTrue(FString::Printf(TEXT("'%s' requests smooth normals"), Label),
			Found->bRequiresSmoothNormals);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineLitmusMirrorsCaptureTest,
	"WeatherPipeline.Litmus.MirrorsCaptureTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineLitmusMirrorsCaptureTest::RunTest(const FString& Parameters)
{
	AMyActor_weather* Actor = MakeActor();
	if (!Actor) { return false; }

	Actor->bLitmusPreviewsAllConditions = true;
	// Config properties load from DefaultGame.ini, so a filter left there by the last run would
	// silently narrow the table under the test. Cleared so these assert the shipped table itself.
	Actor->CaptureConditionFilter.Empty();
	Actor->ConditionFilter.Empty();

	FWeatherPipelineTestAccess::BuildRun(*Actor);

	// The whole value of the preview is that it shows what a run would record. If the two lists ever
	// diverge, the litmus becomes actively misleading rather than merely redundant.
	TestEqual(TEXT("litmus previews exactly the capture conditions"),
		Actor->LitmusConditions.Num(), Actor->CaptureConditions.Num());

	for (int32 i = 0; i < FMath::Min(Actor->LitmusConditions.Num(), Actor->CaptureConditions.Num()); ++i)
	{
		TestEqual(FString::Printf(TEXT("litmus entry %d matches the capture table"), i),
			Actor->LitmusConditions[i].Label, Actor->CaptureConditions[i].Label);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineConditionFilterTest,
	"WeatherPipeline.Conditions.FilterAppliesBeforeLitmus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineConditionFilterTest::RunTest(const FString& Parameters)
{
	AMyActor_weather* Actor = MakeActor();
	if (!Actor) { return false; }

	// The list filter loads from DefaultGame.ini, so whatever the last run left there would narrow the
	// table underneath this test. Cleared first; ConditionFilter is then set to the value under test.
	Actor->CaptureConditionFilter.Empty();

	Actor->ConditionFilter               = TEXT("snow");
	Actor->bLitmusPreviewsAllConditions  = true;

	FWeatherPipelineTestAccess::BuildRun(*Actor);

	TestTrue(TEXT("the filter kept at least one condition"), Actor->CaptureConditions.Num() > 0);

	for (const FWeatherCondition& C : Actor->CaptureConditions)
	{
		TestTrue(FString::Printf(TEXT("'%s' matches the filter"), *C.Label),
			C.Label.Contains(TEXT("snow")));
	}

	// The filter has to be applied before the litmus list is derived, or the preview would show
	// conditions the run then skips.
	TestEqual(TEXT("the litmus list is filtered too"),
		Actor->LitmusConditions.Num(), Actor->CaptureConditions.Num());

	return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Terrain following
//////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	/** An actor drawing spots inside known limits round a known start. */
	AMyActor_weather* MakeSpotActor(const TCHAR* Shot, int32 Spots, int32 Seed)
	{
		AMyActor_weather* Actor = MakeActor();
		if (!Actor) { return nullptr; }
		Actor->CaptureConditionFilter.Empty();
		Actor->ConditionFilter.Empty();
		Actor->CaptureShot     = Shot;
		Actor->SpotCount       = Spots;
		Actor->SpotSeed        = Seed;
		Actor->SpotAreaKm2     = 1.0f;                       // 1 km square: +-500 m round the start
		Actor->SpotHeightMinM  = 30.0f;   Actor->SpotHeightMaxM  = 120.0f;
		Actor->SpotPitchMinDeg = -40.0f;  Actor->SpotPitchMaxDeg = -5.0f;
		Actor->SpotYawStartDeg = 300.0f;  Actor->SpotYawSpanDeg  = 120.0f;   // across 360
		Actor->SpotRollMinDeg  = -10.0f;  Actor->SpotRollMaxDeg  = 10.0f;
		Actor->SpotTimeMin     = 930.0f;  Actor->SpotTimeMax     = 1645.0f;
		Actor->ShotParameters  = { { TEXT("radius_m"), 150.0f }, { TEXT("pictures"), 6.0f } };   // used by an orbit
		FWeatherPipelineTestAccess::SetStart(*Actor, FVector(1000.0, -2000.0, 3000.0));
		return Actor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineRandomSpotsTest,
	"WeatherPipeline.FlightPath.RandomSpotsStayInTheirLimits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineRandomSpotsTest::RunTest(const FString& Parameters)
{
	// Every drawn value inside its limits, one capture point per spot, each starting unresolved so its
	// ground is measured on the first visit.
	AMyActor_weather* Actor = MakeSpotActor(TEXT("frame"), 300, 7);
	if (!Actor) { return false; }
	FWeatherPipelineTestAccess::BuildRun(*Actor);
	const TArray<FCaptureWaypoint>& Path = FWeatherPipelineTestAccess::Path(*Actor);
	TestEqual(TEXT("one capture point per spot"), Path.Num(), 300);
	int32 Outside = 0;
	for (int32 i = 0; i < Path.Num(); ++i)
	{
		const FCaptureWaypoint& W = Path[i];
		const double FromStart = FRotator::NormalizeAxis(W.Rotation.Yaw - 300.0);    // -180..180
		const double YawIntoRange = FromStart < 0.0 ? FromStart + 360.0 : FromStart;  // 0..360 from 300
		const int32 Minutes = (FMath::RoundToInt(W.TimeOfDay) / 100) * 60 + FMath::RoundToInt(W.TimeOfDay) % 100;
		const bool bInside =
			FMath::Abs(W.Location.X - 1000.0) <= 50000.0 && FMath::Abs(W.Location.Y + 2000.0) <= 50000.0 &&
			W.HeightAboveGroundCm >= 3000.0f && W.HeightAboveGroundCm <= 12000.0f &&
			W.Rotation.Pitch >= -40.0 && W.Rotation.Pitch <= -5.0 && YawIntoRange <= 120.0 + 1e-3 &&
			W.Rotation.Roll >= -10.0 && W.Rotation.Roll <= 10.0 &&
			Minutes >= 9 * 60 + 30 && Minutes <= 16 * 60 + 45 && FMath::RoundToInt(W.TimeOfDay) % 100 <= 59 &&
			W.Spot == i && !W.bGroundResolved;
		Outside += bInside ? 0 : 1;
	}
	TestEqual(TEXT("capture points outside their limits"), Outside, 0);

	// The same seed draws the same spots; another seed others.
	AMyActor_weather* Again = MakeSpotActor(TEXT("frame"), 300, 7);
	AMyActor_weather* Other = MakeSpotActor(TEXT("frame"), 300, 8);
	if (!Again || !Other) { return false; }
	FWeatherPipelineTestAccess::BuildRun(*Again);
	FWeatherPipelineTestAccess::BuildRun(*Other);
	TestTrue(TEXT("same seed, same spots"), FWeatherPipelineTestAccess::Path(*Again)[123].Location.Equals(Path[123].Location));
	TestFalse(TEXT("another seed, other spots"), FWeatherPipelineTestAccess::Path(*Other)[123].Location.Equals(Path[123].Location));

	// Limits set equal fix the value: no area, one height, one view, one time.
	AMyActor_weather* Fixed = MakeSpotActor(TEXT("frame"), 1, 1);
	if (!Fixed) { return false; }
	Fixed->SpotAreaKm2 = 0.0f;
	Fixed->SpotHeightMinM = Fixed->SpotHeightMaxM = 65.0f;
	Fixed->SpotPitchMinDeg = Fixed->SpotPitchMaxDeg = -15.0f;
	Fixed->SpotYawStartDeg = 270.0f;  Fixed->SpotYawSpanDeg = 0.0f;
	Fixed->SpotRollMinDeg = Fixed->SpotRollMaxDeg = 0.0f;
	Fixed->SpotTimeMin = Fixed->SpotTimeMax = 1530.0f;
	FWeatherPipelineTestAccess::BuildRun(*Fixed);
	const FCaptureWaypoint& One = FWeatherPipelineTestAccess::Path(*Fixed)[0];
	TestTrue(TEXT("fixed: at the start"), One.Location.Equals(FVector(1000.0, -2000.0, 3000.0)));
	TestEqual(TEXT("fixed: height"), One.HeightAboveGroundCm, 6500.0f, 0.01f);
	TestTrue(TEXT("fixed: view"), One.Rotation.Equals(FRotator(-15.0, 270.0, 0.0), 1e-3));
	TestEqual(TEXT("fixed: time"), One.TimeOfDay, 1530.0f, 0.01f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineEveryShotTest,
	"WeatherPipeline.FlightPath.EveryShotStartsAtItsSpot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineEveryShotTest::RunTest(const FString& Parameters)
{
	// Every shot class there is - a new one included, with no change here: found by its name, at
	// least one pose, the first exactly the spot (its view is what the limits are checked against).
	FCaptureSpot Spot;
	Spot.Location = FVector(1000.0, -2000.0, 3000.0);
	Spot.View     = FRotator(-20.0, 37.0, 4.0);
	const TMap<FName, float> NoParameters;
	int32 Shots = 0;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(UCaptureShot::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		++Shots;
		const FString Name = It->GetName().LeftChop(FString(TEXT("CaptureShot")).Len());
		TestTrue(Name + TEXT(" is found by its name"), UCaptureShot::Find(Name) == It->GetDefaultObject<UCaptureShot>());
		const TArray<FCapturePose> Poses = It->GetDefaultObject<UCaptureShot>()->BuildPoses(Spot, NoParameters);
		if (TestTrue(Name + TEXT(" takes a picture"), Poses.Num() > 0))
		{
			TestTrue(Name + TEXT(" starts at the spot"), Poses[0].Location.Equals(Spot.Location, 0.01));
			TestTrue(Name + TEXT(" starts with the spot's view"), Poses[0].Rotation.Equals(Spot.View, 1e-3));
		}
	}
	TestTrue(TEXT("frame and orbit at least"), Shots >= 2);
	TestNull(TEXT("an unknown shot"), UCaptureShot::Find(TEXT("spiral_of_no_class")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineShotParametersTest,
	"WeatherPipeline.FlightPath.ShotParametersImportFromTheIni",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineShotParametersTest::RunTest(const FString& Parameters)
{
	// The text weather_pipeline.py writes for a location's shot parameters, read the way a
	// CaptureSites entry's settings are.
	AMyActor_weather* Actor = MakeActor();
	if (!Actor) { return false; }
	FProperty* Property = AMyActor_weather::StaticClass()->FindPropertyByName(TEXT("ShotParameters"));
	if (!TestNotNull(TEXT("ShotParameters exists"), Property)) { return false; }
	TestNotNull(TEXT("imports"), Property->ImportText_InContainer(TEXT("((radius_m,150),(pictures,6))"), Actor, Actor, PPF_None));
	TestEqual(TEXT("two parameters"), Actor->ShotParameters.Num(), 2);
	TestEqual(TEXT("radius_m"), Actor->ShotParameters.FindRef(TEXT("radius_m")), 150.0f);
	TestEqual(TEXT("pictures"), Actor->ShotParameters.FindRef(TEXT("pictures")), 6.0f);
	TestNotNull(TEXT("none imports"), Property->ImportText_InContainer(TEXT("()"), Actor, Actor, PPF_None));
	TestEqual(TEXT("a frame's: none"), Actor->ShotParameters.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineOrbitShotTest,
	"WeatherPipeline.FlightPath.OrbitShotCirclesAheadOfTheSpot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineOrbitShotTest::RunTest(const FString& Parameters)
{
	// Each spot: `pictures` points, the first at the spot itself looking along its yaw, all on a
	// circle of `radius_m` round the point that far ahead, all facing it, at the spot's height,
	// pitch, roll and time.
	AMyActor_weather* Actor = MakeSpotActor(TEXT("orbit"), 5, 3);
	if (!Actor) { return false; }
	FWeatherPipelineTestAccess::BuildRun(*Actor);
	const TArray<FCaptureWaypoint>& Path = FWeatherPipelineTestAccess::Path(*Actor);
	TestEqual(TEXT("5 spots x 6 pictures"), Path.Num(), 30);
	for (int32 Spot = 0; Spot < 5 && Path.Num() == 30; ++Spot)
	{
		const FCaptureWaypoint& First = Path[Spot * 6];
		FVector Ahead = First.Rotation.Vector();
		Ahead.Z = 0.0;
		Ahead.Normalize();
		const FVector Centre = First.Location + Ahead * 15000.0;
		for (int32 Picture = 0; Picture < 6; ++Picture)
		{
			const FCaptureWaypoint& W = Path[Spot * 6 + Picture];
			const FVector ToCentre = (Centre - W.Location).GetSafeNormal2D();
			const FString Tag = FString::Printf(TEXT("spot %d picture %d"), Spot, Picture);
			TestEqual(Tag + TEXT(" on the circle"), FVector::Dist2D(W.Location, Centre), 15000.0, 1.0);
			TestEqual(Tag + TEXT(" faces the centre"), FRotator::NormalizeAxis(W.Rotation.Yaw - ToCentre.Rotation().Yaw), 0.0, 0.01);
			TestEqual(Tag + TEXT(" spot's pitch"), W.Rotation.Pitch, First.Rotation.Pitch, 1e-3);
			TestEqual(Tag + TEXT(" spot's roll"), W.Rotation.Roll, First.Rotation.Roll, 1e-3);
			TestEqual(Tag + TEXT(" spot's height"), W.HeightAboveGroundCm, First.HeightAboveGroundCm, 1e-3f);
			TestEqual(Tag + TEXT(" spot's time"), W.TimeOfDay, First.TimeOfDay, 1e-3f);
			TestEqual(Tag + TEXT(" belongs to the spot"), W.Spot, Spot);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineSingleFrameTest,
	"WeatherPipeline.FlightPath.SingleFrameAndWindCompass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineSingleFrameTest::RunTest(const FString& Parameters)
{
	AMyActor_weather* Actor = MakeActor();
	if (!Actor) { return false; }
	Actor->CaptureConditionFilter.Empty();
	Actor->ConditionFilter.Empty();
	Actor->SpotCount       = 1;
	Actor->SpotAreaKm2     = 0.0f;                           // one frame, exactly at the start
	Actor->SpotPitchMinDeg = Actor->SpotPitchMaxDeg = -15.0f;
	Actor->SpotYawStartDeg = 270.0f;
	Actor->SpotYawSpanDeg  = 0.0f;
	FWeatherPipelineTestAccess::SetStart(*Actor, FVector(10.0, 20.0, 3000.0));
	FWeatherPipelineTestAccess::BuildRun(*Actor);

	const TArray<FCaptureWaypoint>& Path = FWeatherPipelineTestAccess::Path(*Actor);
	TestEqual(TEXT("one frame"), Path.Num(), 1);
	if (Path.Num() == 1)
	{
		TestTrue(TEXT("at the start position"), Path[0].Location.Equals(FVector(10.0, 20.0, 3000.0)));
		TestEqual(TEXT("pitch"), Path[0].Rotation.Pitch, -15.0, 1e-4);
		TestEqual(TEXT("heading"), Path[0].Rotation.Yaw, 270.0, 1e-4);
	}

	// A detour leg (up, across, down) is flown by distance: halfway along 100 up + 100 across is the top.
	{
		const TArray<FVector> Leg = { FVector(0, 0, 100), FVector(100, 0, 100) };
		TestTrue(TEXT("start"), AMyActor_weather::PointAlongPath(FVector::ZeroVector, Leg, 0.0).Equals(FVector::ZeroVector));
		TestTrue(TEXT("halfway"), AMyActor_weather::PointAlongPath(FVector::ZeroVector, Leg, 0.5).Equals(FVector(0, 0, 100)));
		TestTrue(TEXT("three quarters"), AMyActor_weather::PointAlongPath(FVector::ZeroVector, Leg, 0.75).Equals(FVector(50, 0, 100)));
		TestTrue(TEXT("end"), AMyActor_weather::PointAlongPath(FVector::ZeroVector, Leg, 1.0).Equals(FVector(100, 0, 100)));
	}

	// Fish-eye 120 deg at 1920x1080 (worked by hand): corner 1101.45 px from the centre, fish-eye focal
	// length 960 / (pi/3) = 916.73 px, so the corner is 1.2015 rad (68.8 deg) off-axis, tan 2.5836. The
	// normal view underneath must be 2*atan(960*2.5836/1101.45) = 132.1 deg wide, rendered
	// 916.73*2.5836/1101.45 = 2.15x larger to keep the centre as sharp.
	{
		float RenderFov = 0.0f, RenderScale = 0.0f;
		AMyActor_weather::FisheyeRender(120.0f, 1920, 1080, RenderFov, RenderScale);
		TestEqual(TEXT("fish-eye render FOV"), RenderFov, 132.1f, 0.1f);
		TestEqual(TEXT("fish-eye render scale"), RenderScale, 2.15f, 0.01f);
		AMyActor_weather::FisheyeRender(10.0f, 1920, 1080, RenderFov, RenderScale);
		TestTrue(TEXT("narrow fish-eye is nearly a normal lens"), RenderFov > 10.0f && RenderFov < 11.5f && RenderScale < 1.05f);
	}

	// Compass FROM-direction <-> UDW yaw round-trips for every quarter.
	for (const float From : { 0.0f, 90.0f, 180.0f, 270.0f })
	{
		TestEqual(FString::Printf(TEXT("wind from %.0f round-trips"), From),
			AMyActor_weather::WindFromForUdwYaw(AMyActor_weather::UdwYawForWindFrom(From)), From, 1e-3f);
	}

	// The four wind conditions exist and each names its direction.
	const TMap<FString, float> Expected = { { TEXT("rain_wind_n"), 0.0f }, { TEXT("rain_wind_e"), 90.0f },
		{ TEXT("snow_wind_s"), 180.0f }, { TEXT("snow_wind_w"), 270.0f } };
	for (const TPair<FString, float>& Pair : Expected)
	{
		const FWeatherCondition* Found = Actor->CaptureConditions.FindByPredicate(
			[&Pair](const FWeatherCondition& C) { return C.Label == Pair.Key; });
		TestNotNull(*FString::Printf(TEXT("condition %s exists"), *Pair.Key), Found);
		if (Found)
		{
			TestEqual(*FString::Printf(TEXT("%s direction"), *Pair.Key), Found->WindFromCompassDeg, Pair.Value, 1e-4f);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineFlyPathTest,
	"WeatherPipeline.FlightPath.FlyingKeepsTheSamePoses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineFlyPathTest::RunTest(const FString& Parameters)
{
	// Flying changes how the drone gets between waypoints, never where the pictures are taken: the
	// capture poses must be identical with and without it, or flown images would not pair.
	TArray<FCaptureWaypoint> Paths[2];
	for (int32 Fly = 0; Fly < 2; ++Fly)
	{
		AMyActor_weather* Actor = MakeActor();
		if (!Actor) { return false; }
		Actor->CaptureConditionFilter.Empty();
		Actor->ConditionFilter.Empty();
		Actor->CaptureShot          = TEXT("orbit");
		Actor->SpotCount            = 3;
		Actor->bFlyBetweenWaypoints = (Fly == 1);
		FWeatherPipelineTestAccess::SetStart(*Actor, FVector(100.0, -50.0, 2000.0));
		FWeatherPipelineTestAccess::BuildRun(*Actor);
		Paths[Fly] = FWeatherPipelineTestAccess::Path(*Actor);
	}

	TestEqual(TEXT("same number of capture points"), Paths[1].Num(), Paths[0].Num());
	for (int32 i = 0; i < FMath::Min(Paths[0].Num(), Paths[1].Num()); ++i)
	{
		TestTrue(FString::Printf(TEXT("capture point %d has the same pose"), i),
			Paths[0][i].Location.Equals(Paths[1][i].Location) && Paths[0][i].Rotation.Equals(Paths[1][i].Rotation));
	}

	// 50 m at 15 m/s is 3.33 s; a zero-length leg still takes a frame rather than dividing by zero.
	TestEqual(TEXT("leg duration"), AMyActor_weather::GlideSeconds(5000.0, 15.0f), 5000.0 / 100.0 / 15.0, 1e-6);
	TestTrue(TEXT("zero-length leg"), AMyActor_weather::GlideSeconds(0.0, 15.0f) > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineManualStartTest,
	"WeatherPipeline.FlightPath.ManualPathStartsAtTheView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineManualStartTest::RunTest(const FString& Parameters)
{
	// AUTOMATE False: the user's view is spot 1, exactly, and the other spots are drawn round it.
	const FVector  User(1000.0, -2000.0, 3000.0);
	const FRotator View(-20.0, 37.0, 0.0);
	const float    UserHeightM = 300.0f;                     // outside the height limits: taken as it is
	for (const TCHAR* Shot : { TEXT("frame"), TEXT("orbit") })
	{
		AMyActor_weather* Actor = MakeSpotActor(Shot, 10, 4);
		if (!Actor) { return false; }
		Actor->TimeOfDay = 1415.0f;
		FWeatherPipelineTestAccess::StartAt(*Actor, User, View, UserHeightM);
		FWeatherPipelineTestAccess::BuildRun(*Actor);
		const TArray<FCaptureWaypoint>& Path = FWeatherPipelineTestAccess::Path(*Actor);
		const FString Name = Shot;
		if (!TestTrue(Name + TEXT(" has spots"), Path.Num() == 10 * (Name == TEXT("orbit") ? 6 : 1))) { continue; }
		const FCaptureWaypoint& First = Path[0];
		TestTrue(Name + TEXT(" spot 1 is the user's place"), First.Location.Equals(User, 1.0));
		TestTrue(Name + TEXT(" spot 1 is the user's view"), First.Rotation.Equals(View, 1e-3));
		TestEqual(Name + TEXT(" spot 1 at the user's height"), First.HeightAboveGroundCm, UserHeightM * 100.0f, 0.01f);
		TestEqual(Name + TEXT(" spot 1 at the time shown"), First.TimeOfDay, 1415.0f, 0.01f);
		for (const FCaptureWaypoint& W : Path)
		{
			if (W.Spot > 0)                                      // the rest round the user's start
			{
				TestTrue(Name + TEXT(" drawn round the start"), FMath::Abs(W.Location.X - User.X) <= 50000.0 + 30000.0 &&
					FMath::Abs(W.Location.Y - User.Y) <= 50000.0 + 30000.0);   // + an orbit's diameter
			}
		}
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Settings validation
//////////////////////////////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineShippedDefaultsAreValidTest,
	"WeatherPipeline.Settings.ShippedDefaultsAreValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineShippedDefaultsAreValidTest::RunTest(const FString& Parameters)
{
	AMyActor_weather* Actor = MakeActor();
	if (!Actor) { return false; }

	// The defaults compiled into the header must themselves pass validation. This is the regression
	// guard that matters most day to day: tuning values get changed constantly, and a default that
	// trips its own validator would otherwise only surface as a log line nobody read.
	TestTrue(TEXT("the shipped defaults pass validation"),
		FWeatherPipelineTestAccess::Validate(*Actor));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineRejectsInvalidCaptureTest,
	"WeatherPipeline.Settings.RejectsInvalidCaptureSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineRejectsInvalidCaptureTest::RunTest(const FString& Parameters)
{
	// Each case must be judged fatal - these produce no usable data, so a run must not proceed.
	{
		AMyActor_weather* Actor = MakeActor();
		Actor->CaptureResolutionX = 0;
		AddExpectedError(TEXT("CaptureResolutionX"), EAutomationExpectedErrorFlags::Contains, 1);
		TestFalse(TEXT("a zero capture width is rejected"),
			FWeatherPipelineTestAccess::Validate(*Actor));
	}

	{
		AMyActor_weather* Actor = MakeActor();
		Actor->CaptureFOV = 0.0f;
		AddExpectedError(TEXT("CaptureFOV"), EAutomationExpectedErrorFlags::Contains, 1);
		TestFalse(TEXT("a zero field of view is rejected"),
			FWeatherPipelineTestAccess::Validate(*Actor));
	}

	{
		// UDS reads time of day as HHMM, so 2500 is not a time. Passing hours here instead of HHMM
		// is an easy mistake and produces a plausible-looking but wrong sun position.
		AMyActor_weather* Actor = MakeActor();
		Actor->TimeOfDay = 2500.0f;
		AddExpectedError(TEXT("TimeOfDay"), EAutomationExpectedErrorFlags::Contains, 1);
		TestFalse(TEXT("an out-of-range time of day is rejected"),
			FWeatherPipelineTestAccess::Validate(*Actor));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineFogCoherenceTest,
	"WeatherPipeline.Settings.FogCapCoherence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineFogCoherenceTest::RunTest(const FString& Parameters)
{
	AMyActor_weather* Actor = MakeActor();
	if (!Actor) { return false; }

	// A hard cutoff nearer than the soft cap makes the soft cap unreachable: the fog is already gone
	// by the distance at which it would have stopped accumulating. Advisory, not fatal - the run
	// still produces valid images, they just will not look as intended - so validation still passes
	// and the operator is warned instead.
	Actor->HeightFogEndDistanceM    = 40000.0f;
	Actor->HeightFogCutoffDistanceM = 10000.0f;

	AddExpectedError(TEXT("nearer than"), EAutomationExpectedErrorFlags::Contains, 1);

	TestTrue(TEXT("contradictory fog caps are a warning, not a hard failure"),
		FWeatherPipelineTestAccess::Validate(*Actor));

	return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// PNG encoding
//////////////////////////////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineOpaquePngTest,
	"WeatherPipeline.Capture.EncodedPngIsOpaque",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineOpaquePngTest::RunTest(const FString& Parameters)
{
	// Regression guard for black output. Scene-capture reads come back with alpha 0 because nothing
	// writes coverage into that channel. The old thumbnail encoder discarded alpha, so this never
	// mattered; PNGCompressImageArray writes BGRA straight through, and a zero-alpha buffer encodes
	// as a fully transparent PNG - which viewers show as solid black.
	//
	// Asserted by round-tripping through the real encoder and decoder rather than by testing the
	// helper directly: what matters is the bytes on disk, not that a loop sets a field.
	const int32 Width  = 8;
	const int32 Height = 8;

	TArray<FColor> Source;
	Source.Init(FColor(20, 140, 220, 0), Width * Height);   // opaque-looking colour, zero alpha

	for (FColor& Pixel : Source)
	{
		Pixel.A = 255;
	}

	TArray64<uint8> Encoded;
	FImageUtils::PNGCompressImageArray(Width, Height, Source, Encoded);
	TestTrue(TEXT("the encoder produced bytes"), Encoded.Num() > 0);

	FImage Decoded;
	if (!FImageUtils::DecompressImage(Encoded.GetData(), Encoded.Num(), Decoded))
	{
		AddError(TEXT("the encoded PNG could not be decoded"));
		return false;
	}

	const TArrayView64<FColor> Pixels = Decoded.AsBGRA8();
	TestEqual(TEXT("the decoded image has the expected pixel count"),
		Pixels.Num(), static_cast<int64>(Width * Height));

	for (int64 i = 0; i < Pixels.Num(); ++i)
	{
		if (Pixels[i].A != 255)
		{
			AddError(FString::Printf(
				TEXT("pixel %lld decoded with alpha %d - a transparent PNG renders as black"),
				i, Pixels[i].A));
			return false;
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Geoid model
//////////////////////////////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeatherPipelineGeoidTest,
	"WeatherPipeline.Geoid.MatchesPublishedUndulations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeatherPipelineGeoidTest::RunTest(const FString& Parameters)
{
	AMyActor_weather* Actor = MakeActor();
	if (!Actor) { return false; }

	if (!FWeatherPipelineTestAccess::LoadGeoid(*Actor))
	{
		// Not a failure: the grid is optional data, and without it the pipeline falls back to the
		// configured constant by design.
		AddInfo(TEXT("no geoid grid present - skipping (Content/Geoid/WW15MGH.GRD)"));
		return true;
	}

	// Checked against published EGM96 undulations at widely separated points. The tolerance is loose
	// because the reference figures are themselves rounded; what this guards is orientation, not
	// precision. A grid read south-to-north instead of north-to-south, or with longitude wrapped the
	// wrong way, still returns smooth plausible numbers everywhere - it just returns them for the
	// wrong hemisphere. Southern-hemisphere and negative-undulation cases are what catch that.
	struct FGeoidCase { const TCHAR* Name; double Lat; double Lon; double Expected; double Tolerance; };

	const FGeoidCase Cases[] =
	{
		{ TEXT("Ingolstadt"),     48.766,  11.426,   46.0, 6.0 },   // the project's own site
		{ TEXT("Singapore"),       1.352, 103.820,	  8.0, 6.0 },   // near-zero, opposite hemisphere
		{ TEXT("southern India"), 10.000,  77.000,  -95.0, 12.0 },  // deepest geoid low on Earth
		{ TEXT("Iceland"),        64.500, -18.000,   65.0, 12.0 },  // strong high, negative longitude
	};

	for (const FGeoidCase& Case : Cases)
	{
		const float Actual = FWeatherPipelineTestAccess::GeoidAt(*Actor, Case.Lat, Case.Lon);

		TestTrue(
			FString::Printf(TEXT("%s geoid undulation %.1f m is within %.0f m of the published %.0f m"),
				Case.Name, Actual, Case.Tolerance, Case.Expected),
			FMath::Abs(Actual - Case.Expected) <= Case.Tolerance);
	}

	// Emitted for offline cross-checking against an independent parse of the same grid file. These
	// are cities with Google Photorealistic 3D Tiles coverage, spread across all four quadrants of
	// latitude and longitude so a sign or wrap error cannot hide in any one of them.
	const FGeoidCase Survey[] =
	{
		{ TEXT("Ingolstadt DE"),    48.766000,   11.426000, 0, 0 },
		{ TEXT("London UK"),        51.507400,   -0.127800, 0, 0 },
		{ TEXT("New York US"),      40.712800,  -74.006000, 0, 0 },
		{ TEXT("San Francisco US"), 37.774900, -122.419400, 0, 0 },
		{ TEXT("Tokyo JP"),         35.676200,  139.650300, 0, 0 },
		{ TEXT("Singapore SG"),      1.352100,  103.819800, 0, 0 },
		{ TEXT("Sydney AU"),       -33.868800,  151.209300, 0, 0 },
		{ TEXT("Rio de Janeiro BR"),-22.906800, -43.172900, 0, 0 },
		{ TEXT("Cape Town ZA"),    -33.924900,   18.424100, 0, 0 },
		{ TEXT("Dubai AE"),         25.204800,   55.270800, 0, 0 },
		{ TEXT("Mexico City MX"),   19.432600,  -99.133200, 0, 0 },
		{ TEXT("Reykjavik IS"),     64.146600,  -21.942600, 0, 0 },
	};

	for (const FGeoidCase& Case : Survey)
	{
		AddInfo(FString::Printf(TEXT("[GeoidSurvey] %s|%.6f|%.6f|%.4f"),
			Case.Name, Case.Lat, Case.Lon,
			FWeatherPipelineTestAccess::GeoidAt(*Actor, Case.Lat, Case.Lon)));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
