// Startup validation for AMyActor_weather's configuration surface.
//
// The actor exposes ~80 tunable properties across Config, the Details panel and per-condition
// overrides. Most failures in this pipeline's history were not crashes - they were a value that was
// silently out of range, in the wrong unit, or contradicted by another value, producing images that
// looked plausible and were wrong. A dataset built from those is worse than no dataset, because
// nothing downstream flags it.
//
// This file is the one place that answers "are the settings coherent?" before a run starts. It only
// reads and reports; it never repairs. Silently correcting a value would hide the mistake and make
// the captured metadata disagree with the settings the user believes they configured.
//
// Kept in its own translation unit deliberately: MyActor_weather.cpp is already long, and validation
// has no reason to be entangled with the apply path.

#include "MyActor_weather.h"
#include "CaptureShots.h"
#include "Components/ExponentialHeightFogComponent.h"

namespace
{
	/** One validation problem. Severity decides whether the run is merely suspect or actually broken. */
	struct FSettingIssue
	{
		bool    bFatal = false;   // true: the run cannot produce valid data, not merely odd-looking
		FString Message;
	};

	/** Range check for a value that must sit inside [Min, Max]. */
	void CheckRange(TArray<FSettingIssue>& Issues, const TCHAR* Name, float Value,
	                float Min, float Max, const TCHAR* Unit, bool bFatal = false)
	{
		if (Value < Min || Value > Max)
		{
			Issues.Add({ bFatal, FString::Printf(
				TEXT("%s = %.4f %s is outside the sane range [%.4f, %.4f]."),
				Name, Value, Unit, Min, Max) });
		}
	}

	/** Positive-value check for a quantity where zero or negative is meaningless (not "disabled"). */
	void CheckPositive(TArray<FSettingIssue>& Issues, const TCHAR* Name, float Value,
	                   const TCHAR* Unit, bool bFatal = false)
	{
		if (Value <= 0.0f)
		{
			Issues.Add({ bFatal, FString::Printf(
				TEXT("%s = %.4f %s must be greater than zero."), Name, Value, Unit) });
		}
	}
}

bool AMyActor_weather::ValidateSettings()
{
	TArray<FSettingIssue> Issues;

	// --- Capture output -------------------------------------------------------------------------
	// Fatal: a zero-sized or absurd render target produces no usable image, and the run would
	// otherwise proceed for hours writing them.
	CheckRange(Issues, TEXT("CaptureResolutionX"), CaptureResolutionX, 64, 8192, TEXT("px"), true);
	CheckRange(Issues, TEXT("CaptureResolutionY"), CaptureResolutionY, 64, 8192, TEXT("px"), true);
	CheckRange(Issues, TEXT("CaptureFOV"),         CaptureFOV,         5.0f, 170.0f, TEXT("deg"), true);

	// --- Time of day ----------------------------------------------------------------------------
	// UDS expresses time as HHMM, so 1360 is not a time and 2500 is not a time.
	if (TimeOfDay < 0.0f || TimeOfDay > 2400.0f)
	{
		Issues.Add({ true, FString::Printf(
			TEXT("TimeOfDay = %.1f is outside 0-2400. UDS reads this as HHMM, not hours."), TimeOfDay) });
	}
	else if (FMath::Fmod(TimeOfDay, 100.0f) > 59.0f)
	{
		Issues.Add({ false, FString::Printf(
			TEXT("TimeOfDay = %.1f has a minutes field above 59. HHMM means 1360 is not 14:00."), TimeOfDay) });
	}

	// --- Fog ------------------------------------------------------------------------------------
	// Density and falloff are coupled: falloff decides how fast density thins with height, so a
	// steep falloff with a low density leaves the scene hazeless, and the pair has to be read
	// together. UDS's own authored falloffs sit near 0.05, far below the 0.5 its slider allows -
	// so a value an order of magnitude above that is worth flagging even though it is legal.
	CheckRange(Issues, TEXT("BaseFogDensity"), BaseFogDensity, 0.0f, 0.5f, TEXT(""));

	auto CheckFalloff = [&Issues](const TCHAR* Name, float Value)
	{
		if (Value < 0.0f)
		{
			Issues.Add({ false, FString::Printf(TEXT("%s = %.4f is negative; 0 means "
				"'leave UDS's own value', anything below that is meaningless."), Name, Value) });
		}
		else if (Value > 0.5f)
		{
			Issues.Add({ false, FString::Printf(TEXT("%s = %.4f exceeds UDS's slider maximum of 0.5 "
				"(its authored values are near 0.05). Reflection writes bypass the clamp, so this "
				"will apply - but expect the fog to collapse into a very shallow layer."), Name, Value) });
		}
	};
	CheckFalloff(TEXT("HeightFogFalloffClear"),  HeightFogFalloffClear);
	CheckFalloff(TEXT("HeightFogFalloffCloudy"), HeightFogFalloffCloudy);
	CheckFalloff(TEXT("HeightFogFalloffFoggy"),  HeightFogFalloffFoggy);

	// A hard cutoff nearer than the soft cap makes the soft cap unreachable - the fog is already
	// gone by the time it would have stopped integrating.
	if (HeightFogCutoffDistanceM > 0.0f && HeightFogEndDistanceM > 0.0f &&
	    HeightFogCutoffDistanceM < HeightFogEndDistanceM)
	{
		Issues.Add({ false, FString::Printf(
			TEXT("HeightFogCutoffDistanceM (%.0fm) is nearer than HeightFogEndDistanceM (%.0fm), so the "
			     "soft cap can never take effect. Use one or the other."),
			HeightFogCutoffDistanceM, HeightFogEndDistanceM) });
	}

	// --- Random spots ---------------------------------------------------------------------------
	const TPair<const TCHAR*, FVector2D> Ranges[] = {
		{ TEXT("SpotHeight (m)"), FVector2D(SpotHeightMinM, SpotHeightMaxM) },
		{ TEXT("SpotPitch (deg)"), FVector2D(SpotPitchMinDeg, SpotPitchMaxDeg) },
		{ TEXT("SpotRoll (deg)"), FVector2D(SpotRollMinDeg, SpotRollMaxDeg) },
		{ TEXT("SpotTime (HHMM)"), FVector2D(SpotTimeMin, SpotTimeMax) } };
	for (const TPair<const TCHAR*, FVector2D>& Range : Ranges)
	{
		if (Range.Value.X > Range.Value.Y)
		{
			Issues.Add({ true, FString::Printf(TEXT("%s: the low end %.1f is above the high end %.1f."),
				Range.Key, Range.Value.X, Range.Value.Y) });
		}
	}
	if (SpotHeightMinM <= 0.0f)
	{
		Issues.Add({ true, FString::Printf(TEXT("SpotHeightMinM is %.1f m; the camera must be above the ground."), SpotHeightMinM) });
	}

	// --- Clouds ---------------------------------------------------------------------------------
	if (CloudSpeed < 0.0f)
	{
		Issues.Add({ false, TEXT("CloudSpeed is negative; 0 means 'leave UDS's own value'.") });
	}
	if (CloudFormationChangeSpeed > 1.5f)
	{
		Issues.Add({ false, FString::Printf(TEXT("CloudFormationChangeSpeed = %.2f exceeds UDS's "
			"maximum of 1.5."), CloudFormationChangeSpeed) });
	}

	// The altitude that actually gets used depends on the mode, so only the relevant one is checked -
	// warning about CloudAltitudeM while the mode is AboveDrone would be noise.
	switch (CloudAltitudeMode)
	{
	case ECloudAltitudeMode::RealWorldAltitude:
		// Above the Karman line the deck is not a cloud layer any more; below sea level it is inside
		// the terrain. Neither is a crash, both are certainly a mistake.
		CheckRange(Issues, TEXT("CloudAltitudeM"), CloudAltitudeM, -500.0f, 100000.0f, TEXT("m"));
		break;

	case ECloudAltitudeMode::UsePreset:
		// Nothing to check: the pipeline writes no cloud altitude at all, so none of the altitude
		// settings are read and warning about them would be noise.
		break;

	case ECloudAltitudeMode::AboveDrone:
		CheckPositive(Issues, TEXT("CloudHeightAboveDroneM"), CloudHeightAboveDroneM, TEXT("m"));
		break;

	case ECloudAltitudeMode::AboveGround:
	case ECloudAltitudeMode::AboveTerrainAtStart:
		CheckPositive(Issues, TEXT("CloudHeightAboveGroundM"), CloudHeightAboveGroundM, TEXT("m"));
		// Both ground-referenced modes depend on the downward trace finding terrain. If the trace
		// range is shorter than the drone's height above ground it silently falls back, which is
		// exactly the "no ground found" warning seen over mountains.
		CheckPositive(Issues, TEXT("GroundTraceDistanceM"), GroundTraceDistanceM, TEXT("m"), true);
		break;
	}

	// --- Cesium ---------------------------------------------------------------------------------
	// A low threshold would let captures proceed over half-streamed terrain, which produces images
	// that look like a successful run and show geometry that was never there.
	CheckRange(Issues, TEXT("CesiumLoadCompleteThresholdPercent"),
	           CesiumLoadCompleteThresholdPercent, 95.0f, 100.0f, TEXT("%"));

	// --- Litmus ---------------------------------------------------------------------------------
	// A non-positive hold is clamped at apply time rather than failing, but the user should know the
	// configured value is not the one being used.
	if (LitmusStepSeconds <= 0.0f)
	{
		Issues.Add({ false, FString::Printf(TEXT("LitmusStepSeconds = %.2f is not positive; it will be "
			"clamped to 0.05s per condition."), LitmusStepSeconds) });
	}
	if (LitmusGapSeconds < 0.0f)
	{
		Issues.Add({ false, TEXT("LitmusGapSeconds is negative; 0 means 'cut straight through'.") });
	}

	// --- Flight path ----------------------------------------------------------------------------
	if (bUseFlightPath && !UCaptureShot::Find(CaptureShot))
	{
		Issues.Add({ true, FString::Printf(TEXT("CaptureShot '%s' has no class U<Name>CaptureShot (see CaptureShots.h)."),
			*CaptureShot) });
	}

	// --- Report ---------------------------------------------------------------------------------
	int32 FatalCount = 0;
	for (const FSettingIssue& Issue : Issues)
	{
		if (Issue.bFatal)
		{
			++FatalCount;
			UE_LOG(LogTemp, Error, TEXT("[Settings] %s"), *Issue.Message);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Settings] %s"), *Issue.Message);
		}
	}

	if (Issues.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[Settings] All checked values are in range."));
	}
	else
	{
		UE_LOG(LogTemp, Log,
			TEXT("[Settings] %d issue(s): %d fatal, %d advisory. Fatal issues mean the captured data "
			     "would not be valid; advisory ones only mean the result may not look as intended."),
			Issues.Num(), FatalCount, Issues.Num() - FatalCount);
	}

	return FatalCount == 0;
}

void AMyActor_weather::ReportLiveAltitude(float DeltaTime)
{
	// Only while idle. During a run the capture log is the record of what happened, and half a line
	// of telemetry every 0.5s would bury it.
	if (!bLogLiveAltitude || IsSequenceRunning() || !TargetThirdPersonCharacter)
	{
		return;
	}

	LiveAltitudeAccumulator += DeltaTime;
	if (LiveAltitudeAccumulator < FMath::Max(LiveAltitudeIntervalSeconds, 0.05f))
	{
		return;
	}
	LiveAltitudeAccumulator = 0.0f;

	const FVector DronePos = TargetThirdPersonCharacter->GetActorLocation();

	// 1. Real-world altitude: metres above the WGS84 ellipsoid, straight from the Cesium
	//    georeference. Absolute - the same number means the same height anywhere on the globe, and
	//    it is the figure a real flight log would quote.
	FVector DroneGeo(0.0);
	const bool bHaveGeo = GetGeographicPosition(DronePos, DroneGeo);

	// Looked up per position from the EGM96 grid, so this stays correct if the georeference moves.
	// Cesium returns lon/lat/height, so X is longitude and Y is latitude.
	const float Separation = bHaveGeo ? GetGeoidSeparationAt(DroneGeo.Y, DroneGeo.X) : 0.0f;

	// 2. The ground's own elevation at this location, also above mean sea level - the figure that
	//    can be checked against a map. Takeoff-relative height was tried here and removed: spawning
	//    airborne makes it read zero at altitude, which describes the session rather than the place.
	float GroundZ = 0.0f, HeightAboveGroundCm = 0.0f;
	const bool bHaveGround = MeasureGroundBelowDrone(GroundZ, HeightAboveGroundCm);

	FVector GroundGeo(0.0);
	const bool bHaveGroundGeo = bHaveGround && bHaveGeo &&
		GetGeographicPosition(FVector(DronePos.X, DronePos.Y, GroundZ), GroundGeo);

	// Both datums, so the ellipsoid figure Cesium actually produced stays visible next to the
	// mean-sea-level figure that matches published elevations. Showing only the converted value
	// would hide which one the pipeline is really working in.
	const FString AltitudeText = bHaveGeo
		? (Separation != 0.0f
			? FString::Printf(TEXT("%.1f m MSL (%.1f m ellipsoid, geoid %+.1f m)"),
				DroneGeo.Z - Separation, DroneGeo.Z, Separation)
			: FString::Printf(TEXT("%.1f m ellipsoid"), DroneGeo.Z))
		: FString(TEXT("unavailable (no Cesium georeference)"));

	// Same datum as the drone's altitude, so the two are directly comparable and their difference is
	// the clearance - without either number having to be relative to the other.
	const FString GroundText = bHaveGroundGeo
		? FString::Printf(TEXT("%.1f m MSL"), GroundGeo.Z - Separation)
		: FString(TEXT("unavailable (no terrain resolved yet)"));

	// Coordinates included so a reading can be checked against published elevation data - without
	// them the numbers are unverifiable, since there is no way to say which point they describe.
	// Six decimal places is about 0.1 m, finer than the terrain sample itself.
	const FString CoordText = bHaveGeo
		? FString::Printf(TEXT("%.6f, %.6f"), DroneGeo.Y, DroneGeo.X)   // lat, lon
		: FString(TEXT("--"));

	UE_LOG(LogTemp, Log,
		TEXT("[Altitude] at %s | drone %s | ground %s"),
		*CoordText, *AltitudeText, *GroundText);

	if (bDrawLiveAltitudeOnScreen && GEngine)
	{
		// A fixed key so each update replaces the previous line rather than stacking a new one.
		GEngine->AddOnScreenDebugMessage(
			/*Key=*/ 0x5744414C, FMath::Max(LiveAltitudeIntervalSeconds, 0.05f) * 2.0f,
			FColor::Cyan,
			FString::Printf(TEXT("Drone %s   |   Ground %s"), *AltitudeText, *GroundText));
	}
}

void AMyActor_weather::LogEnvironmentCapabilities()
{
	// Which optional systems this world actually provides. The pipeline is written against Cesium
	// but does not depend on it at compile time - every Cesium call goes through a reflected class
	// path that resolves to null elsewhere. This line makes that explicit rather than leaving it to
	// be inferred from the absence of warnings, so running in a non-Cesium level is a stated,
	// verified configuration rather than an untested one.
	const bool bHasGeoreference = LoadObject<UClass>(nullptr, TEXT("/Script/CesiumRuntime.CesiumGeoreference")) != nullptr;
	const bool bHasTilesets     = LoadObject<UClass>(nullptr, TEXT("/Script/CesiumRuntime.Cesium3DTileset")) != nullptr;
	const bool bHasUDS          = UDSActor.IsValid();
	const bool bHasUDW          = UDWActor.IsValid();

	UE_LOG(LogTemp, Log,
		TEXT("[Environment] Cesium georeference: %s | Cesium tilesets: %s | Ultra Dynamic Sky: %s | "
		     "Ultra Dynamic Weather: %s"),
		bHasGeoreference ? TEXT("yes") : TEXT("no"),
		bHasTilesets     ? TEXT("yes") : TEXT("no"),
		bHasUDS          ? TEXT("yes") : TEXT("no"),
		bHasUDW          ? TEXT("yes") : TEXT("no"));

	// UDS/UDW are the only hard requirements - without them there is no weather to drive at all.
	if (!bHasUDS || !bHasUDW)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[Environment] Ultra Dynamic Sky and Weather are both required. Place them in the level."));
	}

	// Without Cesium, real-world altitude has no meaning, so the one mode that needs a globe cannot
	// work. Say so rather than silently falling back and recording altitudes that are really just
	// world Z in disguise.
	if (!bHasGeoreference && CloudAltitudeMode == ECloudAltitudeMode::RealWorldAltitude)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Environment] CloudAltitudeMode is RealWorldAltitude but this world has no Cesium "
			     "georeference, so there is no ellipsoid to measure against. Falling back to height "
			     "above terrain. Use AboveTerrainAtStart explicitly in non-Cesium levels."));
	}
}
