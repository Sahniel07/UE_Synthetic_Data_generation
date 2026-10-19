// EGM96 geoid lookup, so real-world altitudes can be reported above mean sea level anywhere on the
// globe rather than above the WGS84 ellipsoid.
//
// Cesium reports ellipsoid height; published elevations, altimeters and charts are orthometric. The
// two differ by the geoid undulation - about +45 m over Bavaria, +7 m over Singapore, and roughly
// -30 m over southern India. A single configured constant is exact for one city and badly wrong the
// moment the georeference moves, which is why this exists.
//
// Reads NGA's WW15MGH.GRD: a plain ASCII grid whose six-number header gives the latitude and
// longitude bounds and the step, followed by the undulations in metres. The values are used as
// published rather than recomputed from spherical harmonics - synthesising EGM96 to degree 360 means
// ~130,000 associated Legendre evaluations per point, is easy to get subtly wrong, and buys nothing
// over the grid, which is already accurate to a few centimetres against the full model.

#include "MyActor_weather.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "GameFramework/Character.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Kismet/GameplayStatics.h"
#include "UDWBridge.h"
#include "EngineUtils.h"

#if WITH_CESIUM_RUNTIME
#include "Cesium3DTileset.h"
#include "CesiumSampleHeightResult.h"
#endif

namespace
{
	/**
	 * Reads the next whitespace-delimited number. Hand-rolled rather than ParseIntoArray + Atof
	 * because the file holds over a million values: splitting it would allocate a million FStrings
	 * for a load that should be a single linear scan.
	 */
	bool ReadNextDouble(const ANSICHAR*& P, const ANSICHAR* End, double& Out)
	{
		while (P < End && (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n')) { ++P; }
		if (P >= End) { return false; }

		const ANSICHAR* Start = P;
		while (P < End && *P != ' ' && *P != '\t' && *P != '\r' && *P != '\n') { ++P; }

		const int32 Len = static_cast<int32>(P - Start);
		if (Len <= 0 || Len >= 64) { return false; }

		ANSICHAR Token[64];
		FMemory::Memcpy(Token, Start, Len);
		Token[Len] = '\0';

		Out = FCStringAnsi::Atod(Token);
		return true;
	}
}

bool AMyActor_weather::LoadGeoidGrid() const
{
	// Attempted once per session either way: a missing file should produce one warning, not one per
	// frame of the live altitude readout.
	if (bGeoidLoadAttempted)
	{
		return GeoidGrid.Num() > 0;
	}
	bGeoidLoadAttempted = true;

	const FString Path = GeoidGridPath.IsEmpty()
		? FPaths::ProjectContentDir() / TEXT("Geoid/WW15MGH.GRD")
		: GeoidGridPath;

	TArray<uint8> Raw;
	if (!FFileHelper::LoadFileToArray(Raw, *Path))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Geoid] Could not read '%s'. Falling back to the fixed GeoidSeparationM (%.1f m), "
			     "which is only valid near the location it was calibrated for."),
			*Path, GeoidSeparationM);
		return false;
	}

	const ANSICHAR* P   = reinterpret_cast<const ANSICHAR*>(Raw.GetData());
	const ANSICHAR* End = P + Raw.Num();

	// Header: south, north, west, east, dLat, dLon - all in degrees.
	double South = 0.0, North = 0.0, West = 0.0, East = 0.0, DLat = 0.0, DLon = 0.0;
	if (!ReadNextDouble(P, End, South) || !ReadNextDouble(P, End, North) ||
	    !ReadNextDouble(P, End, West)  || !ReadNextDouble(P, End, East)  ||
	    !ReadNextDouble(P, End, DLat)  || !ReadNextDouble(P, End, DLon)  ||
	    DLat <= 0.0 || DLon <= 0.0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Geoid] '%s' does not start with a readable six-number header."), *Path);
		return false;
	}

	GeoidNorth = North;
	GeoidWest  = West;
	GeoidDLat  = DLat;
	GeoidDLon  = DLon;
	GeoidRows  = FMath::RoundToInt((North - South) / DLat) + 1;
	GeoidCols  = FMath::RoundToInt((East  - West) / DLon) + 1;

	const int32 Expected = GeoidRows * GeoidCols;
	GeoidGrid.Reset();
	GeoidGrid.Reserve(Expected);

	double Value = 0.0;
	while (ReadNextDouble(P, End, Value))
	{
		GeoidGrid.Add(static_cast<float>(Value));
	}

	// A short file would silently skew every lookup toward the end of the grid, so it is rejected
	// rather than used partially. A long one is tolerated - trailing whitespace or a stray line
	// costs nothing, since indexing never reads past Expected.
	if (GeoidGrid.Num() < Expected)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Geoid] '%s' holds %d values but its header describes %d (%d x %d). Ignoring it."),
			*Path, GeoidGrid.Num(), Expected, GeoidRows, GeoidCols);
		GeoidGrid.Reset();
		return false;
	}

	UE_LOG(LogTemp, Log,
		TEXT("[Geoid] Loaded %s: %d x %d grid at %.3f deg, lat %.1f..%.1f, lon %.1f..%.1f. "
		     "Altitudes are now reported above mean sea level worldwide."),
		*FPaths::GetCleanFilename(Path), GeoidRows, GeoidCols, DLat, South, North, West, East);

	return true;
}

float AMyActor_weather::GetGeoidSeparationAt(double LatitudeDeg, double LongitudeDeg) const
{
	if (!LoadGeoidGrid())
	{
		return GeoidSeparationM;   // configured constant, or 0 for raw ellipsoid height
	}

	// The grid is published over 0..360 east longitude; Cesium hands back -180..180.
	double Lon = FMath::Fmod(LongitudeDeg - GeoidWest, 360.0);
	if (Lon < 0.0) { Lon += 360.0; }

	// Rows run north to south, which is why this subtracts from the north edge rather than adding
	// to the south one - getting that backwards mirrors the entire model about the equator and
	// still produces plausible-looking numbers.
	const double RowF = (GeoidNorth - LatitudeDeg) / GeoidDLat;
	const double ColF = Lon / GeoidDLon;

	const int32 Row0 = FMath::Clamp(FMath::FloorToInt(RowF), 0, GeoidRows - 1);
	const int32 Col0 = FMath::Clamp(FMath::FloorToInt(ColF), 0, GeoidCols - 1);
	const int32 Row1 = FMath::Min(Row0 + 1, GeoidRows - 1);
	const int32 Col1 = FMath::Min(Col0 + 1, GeoidCols - 1);

	const double FracRow = FMath::Clamp(RowF - Row0, 0.0, 1.0);
	const double FracCol = FMath::Clamp(ColF - Col0, 0.0, 1.0);

	const float V00 = GeoidGrid[Row0 * GeoidCols + Col0];
	const float V01 = GeoidGrid[Row0 * GeoidCols + Col1];
	const float V10 = GeoidGrid[Row1 * GeoidCols + Col0];
	const float V11 = GeoidGrid[Row1 * GeoidCols + Col1];

	// Bilinear. Over a 0.25 degree cell the undulation is smooth enough that this sits within a few
	// centimetres of the full spherical-harmonic model.
	const double Top    = FMath::Lerp<double>(V00, V01, FracCol);
	const double Bottom = FMath::Lerp<double>(V10, V11, FracCol);

	return static_cast<float>(FMath::Lerp<double>(Top, Bottom, FracRow));
}

void AMyActor_weather::AnchorFogPlane()
{
	if (!bAnchorFogPlaneToAltitude || !TargetThirdPersonCharacter)
	{
		return;
	}

	UExponentialHeightFogComponent* Fog = GetSkyHeightFogComponent();
	if (!Fog)
	{
		return;
	}

	const FVector Reference = TargetThirdPersonCharacter->GetActorLocation();

	FVector Geo(0.0);
	if (!GetGeographicPosition(Reference, Geo))
	{
		return;   // No georeference: leave the plane where UDS put it rather than guess.
	}

	// Follow the ground by default, so the plane travels with the georeference instead of staying
	// at a number that was only ever right for one city.
	float PlaneMSL = FogPlaneAltitudeM;
	if (bFogPlaneFollowsSiteGround)
	{
		if (SiteGroundElevationM <= 0.0f)
		{
			// No ground elevation configured. Leave the plane where UDS put it rather than anchoring
			// to a guess, which would drop the fog hundreds of metres and look like a render fault.
			return;
		}

		PlaneMSL = SiteGroundElevationM + FogPlaneOffsetM;
	}

	// PlaneMSL is orthometric; the conversion wants height above the ellipsoid, so the undulation
	// has to be added back on - the same step the ground elevation needs, and the same 46 m error at
	// Ingolstadt if it were skipped.
	const float  Undulation = GetGeoidSeparationAt(Geo.Y, Geo.X);
	double       TargetZ    = 0.0;
	if (!GetUnrealZForAltitude(Reference, PlaneMSL + Undulation, TargetZ))
	{
		return;
	}

	const FVector Current = Fog->GetComponentLocation();

	// Only move it when it has actually drifted. SetWorldLocation dirties the render state, and this
	// runs on every weather step.
	if (FMath::IsNearlyEqual(Current.Z, TargetZ, 1.0))
	{
		return;
	}

	Fog->SetWorldLocation(FVector(Current.X, Current.Y, TargetZ));

	if (!bLoggedFogPlaneAnchor)
	{
		bLoggedFogPlaneAnchor = true;
		UE_LOG(LogTemp, Log,
			TEXT("Height fog plane anchored to %.0fm MSL (%s, geoid %+.1fm, world Z %.0fcm, was %.0fcm). "
			     "Fog density is now referenced to a real-world altitude rather than world Z=0, so it "
			     "reads the same at any location on the globe."),
			PlaneMSL,
			bFogPlaneFollowsSiteGround
				? TEXT("following site ground")
				: TEXT("fixed altitude"),
			Undulation, TargetZ, Current.Z);
	}
}

void AMyActor_weather::AnchorSkyAtmosphereToGlobe()
{
	if (!bCurveSkyAtmosphereToGlobe || !TargetThirdPersonCharacter)
	{
		return;
	}

	AActor* UDS = UDSActor.Get();
	if (!UDS)
	{
		return;
	}

	USkyAtmosphereComponent* Atmosphere = UDS->FindComponentByClass<USkyAtmosphereComponent>();
	if (!Atmosphere)
	{
		return;
	}

	// The Earth's centre in Unreal coordinates. Asked of the georeference rather than derived from a
	// radius constant, so it stays correct whatever ellipsoid and origin the level is configured
	// with - the same source ACesiumSunSky uses by parenting itself to a globe anchor at the centre.
	UClass* GeoClass = LoadObject<UClass>(nullptr, TEXT("/Script/CesiumRuntime.CesiumGeoreference"));
	AActor* Georeference = GeoClass ? UGameplayStatics::GetActorOfClass(GetWorld(), GeoClass) : nullptr;
	if (!Georeference)
	{
		return;   // No globe to curve to; leaving UDS's flat anchor is the correct behaviour.
	}

	static const FName EcefToUnreal(TEXT("TransformEarthCenteredEarthFixedPositionToUnreal"));

	FVector EarthCentre(0.0);
	if (!UDWBridge::CallVectorToVector(Georeference, EcefToUnreal, FVector::ZeroVector, EarthCentre))
	{
		return;   // Bridge has already logged the miss.
	}

	// Ground radius: the distance from that centre to the terrain beneath the drone. Measured rather
	// than assumed, so it is the local ellipsoid radius at this latitude instead of an equatorial
	// constant - the ellipsoid varies by 21 km between equator and pole, far more than the cloud
	// layer's own thickness.
	const FVector DronePos = TargetThirdPersonCharacter->GetActorLocation();

	float GroundZ = 0.0f, HeightAboveGround = 0.0f;
	const bool bHaveGround = MeasureGroundBelowDrone(GroundZ, HeightAboveGround);
	const FVector GroundPos(DronePos.X, DronePos.Y, bHaveGround ? GroundZ : DronePos.Z);

	const double RadiusKm = FVector::Dist(EarthCentre, GroundPos) / 100000.0;   // cm -> km
	if (RadiusKm < 1.0 || RadiusKm > 10000.0)
	{
		// Outside the component's own clamp range, so the conversion produced something wrong.
		// Better to leave the sky alone than to anchor it somewhere absurd.
		UE_LOG(LogTemp, Warning,
			TEXT("[SkyGlobe] Computed ground radius %.1f km is implausible; leaving the atmosphere as "
			     "UDS configured it."), RadiusKm);
		return;
	}

	const FVector PreviousLocation = Atmosphere->GetComponentLocation();

	Atmosphere->TransformMode = ESkyAtmosphereTransformMode::PlanetCenterAtComponentTransform;
	Atmosphere->SetWorldLocation(EarthCentre);
	Atmosphere->SetBottomRadius(static_cast<float>(RadiusKm));
	Atmosphere->MarkRenderStateDirty();

	if (!bLoggedSkyAtmosphereAnchor)
	{
		bLoggedSkyAtmosphereAnchor = true;
		UE_LOG(LogTemp, Log,
			TEXT("[SkyGlobe] Atmosphere anchored to the globe: planet centre at (%.0f, %.0f, %.0f), "
			     "ground radius %.1f km, mode PlanetCenterAtComponentTransform (was at %.0f,%.0f,%.0f). "
			     "Volumetric clouds are positioned above the planet surface, so they now curve with "
			     "the Earth and meet the horizon instead of sitting as a flat ceiling."),
			EarthCentre.X, EarthCentre.Y, EarthCentre.Z, RadiusKm,
			PreviousLocation.X, PreviousLocation.Y, PreviousLocation.Z);
	}
}

void AMyActor_weather::ApplyGeoreferenceOrigin()
{
	if (!bSetGeoreferenceOrigin)
	{
		return;   // Level keeps whatever origin it was authored with.
	}

	// Rejected rather than clamped: a longitude of 250 is far more likely to be the 0..360 convention
	// used by the geoid grid than a genuine intent, and silently wrapping it would move the level
	// somewhere plausible-looking but wrong.
	if (GeoreferenceOriginLatitude < -90.0 || GeoreferenceOriginLatitude > 90.0 ||
	    GeoreferenceOriginLongitude < -180.0 || GeoreferenceOriginLongitude > 180.0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Site] Origin %.6f, %.6f is out of range (lat -90..90, lon -180..180, west "
			     "negative). Leaving the level's own georeference alone."),
			GeoreferenceOriginLatitude, GeoreferenceOriginLongitude);
		return;
	}

	UClass* GeoClass = LoadObject<UClass>(nullptr, TEXT("/Script/CesiumRuntime.CesiumGeoreference"));
	AActor* Georeference = GeoClass ? UGameplayStatics::GetActorOfClass(GetWorld(), GeoClass) : nullptr;
	if (!Georeference)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Site] bSetGeoreferenceOrigin is on but this level has no CesiumGeoreference actor. "
			     "The site coordinates will move the sun but not the terrain."));
		return;
	}

	// SetOriginLongitudeLatitudeHeight takes LONGITUDE first, then latitude, then ellipsoid height -
	// the opposite order to how coordinates are usually quoted, and the same order as Unreal's X/Y/Z.
	// Getting it backwards puts a European site in the Indian Ocean, so the order is asserted by the
	// log line below rather than trusted.
	static const FName SetOrigin(TEXT("SetOriginLongitudeLatitudeHeight"));
	UFunction* Function = Georeference->FindFunction(SetOrigin);
	if (!Function)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Site] CesiumGeoreference has no '%s'. The Cesium plugin version may have changed "
			     "it; the origin has not been moved."), *SetOrigin.ToString());
		return;
	}

	// Reuse the level's own origin height unless one was explicitly configured. The height is a
	// property of how the level was built, not of the site: overwriting it with 0 slid a level
	// authored at ~10.8 km straight down to the ellipsoid and buried the drone.
	double HeightToUse = GeoreferenceOriginHeightM;
	if (HeightToUse <= KeepLevelOriginHeight)
	{
		HeightToUse = 0.0;
		if (const FDoubleProperty* HeightProp =
			CastField<FDoubleProperty>(Georeference->GetClass()->FindPropertyByName(TEXT("OriginHeight"))))
		{
			HeightToUse = HeightProp->GetPropertyValue_InContainer(Georeference);
		}
	}

	struct FSetOriginParams { FVector TargetLongitudeLatitudeHeight; };
	FSetOriginParams Params;
	Params.TargetLongitudeLatitudeHeight = FVector(
		GeoreferenceOriginLongitude,
		GeoreferenceOriginLatitude,
		HeightToUse);

	Georeference->ProcessEvent(Function, &Params);

	UE_LOG(LogTemp, Log,
		TEXT("[Site] Georeference origin moved to %.6f lat, %.6f lon, %.1f m ellipsoid height (%s). "
		     "Terrain, sun and altitude readouts now all describe the same place."),
		GeoreferenceOriginLatitude, GeoreferenceOriginLongitude, HeightToUse,
		(GeoreferenceOriginHeightM <= KeepLevelOriginHeight)
			? TEXT("kept from the level") : TEXT("configured"));

	// The site's coordinates now sit at the Unreal origin by definition, so that is where the drone
	// belongs. Left where it was, it keeps a world XY that maps to somewhere else entirely - after
	// the first move it ended up 60 km from the site it was supposed to be capturing.
	bRecentreDroneOnSite = true;

	// Keep the sun with the terrain. Setting one without the other is the exact failure this whole
	// function exists to remove, so it is done here rather than left to the caller to remember.
	SkyLatitude  = static_cast<float>(GeoreferenceOriginLatitude);
	SkyLongitude = static_cast<float>(GeoreferenceOriginLongitude);

	// The ground under the drone is now a different place, so its height has to be measured again.
	// Without this the drone keeps the clearance it had at the previous site, which is the whole
	// problem the spawn height exists to solve.
	bSpawnHeightApplied = false;
	bHaveSpawnGround    = false;   // new site, new ground reference
}

// --- Multi-site runs ----------------------------------------------------------------------------

bool AMyActor_weather::ApplyCaptureSite(int32 Index, bool bMoveGlobe)
{
	if (!CaptureSites.IsValidIndex(Index))
	{
		return false;
	}

	const FCaptureSite& Site = CaptureSites[Index];

	if (bMoveGlobe)
	{
		// Route the site's coordinates through the same fields and the same function a single-site run
		// uses, rather than duplicating the move here. One code path means the range checks, the
		// longitude-order handling and the sun sync cannot drift between the two cases.
		bSetGeoreferenceOrigin       = true;
		GeoreferenceOriginLatitude   = Site.Latitude;
		GeoreferenceOriginLongitude  = Site.Longitude;
		GeoreferenceOriginHeightM    = Site.HeightM;
		ApplyGeoreferenceOrigin();

		// Both are built only when empty: cleared, the site's run rebuilds the condition list from its
		// own filter and the path around its own spawn, with every capture point measured afresh.
		// Kept, the second city was captured at the first city's heights.
		CaptureConditions.Reset();
		FlightPath.Reset();
		bHaveManualStartSpot = false;                    // this site's spots are drawn afresh

		// New ground: calculate its height and place the drone again, exactly as at the first site.
		bSceneSetupRequested      = false;
		bOriginFromCalculation    = false;
		OriginCalibratedAtSeconds = -1.0;
		SpawnFallbackElapsed      = 0.0f;
	}

	// The site's own camera: each "Key=Value" goes onto the Config property of that name, exactly as
	// the ini would have set it. Only Config properties, so a site cannot reach internal state.
	for (const FString& Setting : Site.Settings)
	{
		FString Key, Value;
		FProperty* Property = Setting.Split(TEXT("="), &Key, &Value)
			? GetClass()->FindPropertyByName(FName(*Key.TrimStartAndEnd())) : nullptr;
		if (!Property || !Property->HasAnyPropertyFlags(CPF_Config) ||
			!Property->ImportText_InContainer(*Value.TrimStartAndEnd(), this, this, PPF_None))
		{
			UE_LOG(LogTemp, Warning, TEXT("[Site %d/%d] Ignored setting '%s': not a settable Config property."),
				Index + 1, CaptureSites.Num(), *Setting);
		}
	}

	// The site's own image: render quality, tile detail and the capture camera (resolution, field of
	// view, lens, exposure) follow whatever its settings changed. Particle sizes are applied per condition.
	ApplyRenderQuality();
	ConfigureCesiumForCapture();
	SetupCaptureComponent();

	// The sun is solved for the site's own coordinates - the first site too, whose globe is not moved
	// (its coordinates would otherwise stay at the config default and the sun sit over Ingolstadt).
	SkyLatitude  = static_cast<float>(Site.Latitude);
	SkyLongitude = static_cast<float>(Site.Longitude);

	// The site's own clock and conditions; the flight path and condition list are rebuilt from these
	// (and the camera above) when its run starts (StartMatchedCaptureRun).
	if (Site.TimeOfDay >= 0.0f)
	{
		TimeOfDay = Site.TimeOfDay;
	}
	ApplySkySettings(FUDWWeatherStep());
	if (Site.Conditions.Num() > 0)
	{
		CaptureConditionFilter = Site.Conditions;
	}

	// Per-site output folder, WeatherSnapshots/<Name>. Without it the second site overwrites the
	// first: filenames are built from condition and waypoint, neither of which knows the location.
	OutputSubfolder = Site.Name.IsEmpty() ? FString::Printf(TEXT("site_%02d"), Index) : Site.Name;
	ResolveOutputDirs();

	UE_LOG(LogTemp, Log,
		TEXT("[Site %d/%d] %s at %.6f, %.6f, time %.0f, %d condition(s). Images -> %s"),
		Index + 1, CaptureSites.Num(), *Site.Name, Site.Latitude, Site.Longitude, TimeOfDay,
		CaptureConditionFilter.Num(), *ResolvedSnapshotDir);

	return true;
}

bool AMyActor_weather::AdvanceToNextSite()
{
	if (CaptureSites.Num() == 0 || CurrentSiteIndex + 1 >= CaptureSites.Num())
	{
		return false;   // single-site run, or the last site just finished
	}

	++CurrentSiteIndex;

	if (!ApplyCaptureSite(CurrentSiteIndex, /*bMoveGlobe=*/true))
	{
		return false;
	}

	// Weather is released while the tiles restream. Holding a fog preset across the move would tint
	// the placeholder terrain and make a streaming delay look like a weather fault.
	ClearUDWWeather(/*bRestoreSkyDefaults=*/false);

	// Manual frame: this site's run is started by P, like the first one - so no sequence is running
	// in between, or P would cancel instead of start.
	if (bManualFrame)
	{
		bPipelineActive = false;
	}

	GetWorldTimerManager().SetTimer(SiteSettleTimerHandle, this,
		&AMyActor_weather::BeginRunAtCurrentSite, FMath::Max(0.1f, SiteSettleSeconds), false);

	return true;
}

void AMyActor_weather::BeginRunAtCurrentSite()
{
	ResetTilesetOffsets();

	if (bManualFrame)
	{
		// As at Play: calculate the ground, place the drone (Tick) and hand the controls back.
		StartGroundHeightCalculation();
		SetPlayerInputLocked(false);
		UE_LOG(LogTemp, Log,
			TEXT("[Site %d/%d] Fly to the view you want and press P to capture %s."),
			CurrentSiteIndex + 1, CaptureSites.Num(), *CaptureSites[CurrentSiteIndex].Name);
		return;
	}

	// Automated: the same setup and run the first site had - ground calculation, spawn, then
	// StartWeatherPipeline, which rebuilds the path and conditions and starts a fresh folder.
	BeginSceneSetupThenRun();
}

void AMyActor_weather::ApplySpawnHeight()
{
	if (!bSpawnAtHeightAboveGround || bSpawnHeightApplied || !TargetThirdPersonCharacter)
	{
		return;
	}

	// Wait for the tiles. A trace into unstreamed terrain either misses entirely or hits a coarse
	// placeholder hundreds of metres off - the source of the nonsense ground readings this project
	// has seen before. Better to place the drone a second late than at a wrong height.
	float LowestProgress = 0.0f;
	if (!AreCesiumTilesetsLoaded(LowestProgress))
	{
		return;
	}

	// The ground height is still being calculated from the tileset: place nothing until it is known.
	if (bGroundCalculationPending)
	{
		return;
	}

	// Just calibrated the origin: give the tiles a moment to move before measuring again.
	if (OriginCalibratedAtSeconds >= 0.0 && GetWorld()->GetTimeSeconds() - OriginCalibratedAtSeconds < 1.0)
	{
		return;
	}

	const FVector Current = TargetThirdPersonCharacter->GetActorLocation();

	// If the globe was moved, put the drone over the site first. The configured coordinates sit at
	// the Unreal origin by definition, so XY zero is exactly the requested location. Done before the
	// ground trace, so the trace samples terrain at the site rather than wherever the drone drifted.
	if (bRecentreDroneOnSite)
	{
		bRecentreDroneOnSite = false;
		TargetThirdPersonCharacter->SetActorLocation(
			FVector(0.0, 0.0, Current.Z), /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);

		return;   // Let the tiles under the new position stream in before measuring.
	}

	// Trace from far ABOVE the drone rather than from the drone downward. A downward trace from the
	// drone's own position finds nothing when the drone starts underground - the terrain is above it,
	// not below - which is exactly the case this function has to recover from. Starting high makes
	// the trace independent of where the drone happens to be.
	// ProbeGroundZ does exactly that, and takes the lowest of several hits so a rooftop under the
	// spawn point does not become "the ground".
	double ProbedZ = 0.0;
	float  GroundZ = 0.0f;

	if (ProbeGroundZ(Current.X, Current.Y, Current.Z, ProbedZ))
	{
		// Put Z = 0 on the ground: the measured offset is exactly how far the origin height is off.
		// Once per session, then measure again after the tiles have moved.
		if (bCalibrateOriginToGround && !bOriginFromCalculation && OriginCalibratedAtSeconds < 0.0 &&
			FMath::Abs(ProbedZ) > 100.0 &&
			ShiftGeoreferenceOriginHeight(ProbedZ / 100.0))
		{
			OriginCalibratedAtSeconds = GetWorld()->GetTimeSeconds();
			return;
		}
		GroundZ          = static_cast<float>(ProbedZ);
		SpawnGroundZ     = ProbedZ;
		bHaveSpawnGround = true;
	}
	else
	{
		// No collision under the drone. Cesium tilesets only generate physics meshes when "Create
		// Physics Meshes" is enabled, and without it every trace passes straight through terrain that
		// is plainly visible - so a failed trace is far more often a tileset setting than a missing
		// tile, and retrying forever would leave the drone underground indefinitely.
		//
		// Fall back to the georeference origin. When the origin has been placed at the site's ground
		// elevation, world Z = 0 IS ground level by construction, so this needs no trace at all. It
		// is an assumption rather than a measurement, so it is logged as one.
		SpawnFallbackElapsed += GetWorld()->GetDeltaSeconds();
		if (SpawnFallbackElapsed < 5.0f)
		{
			return;   // Give genuine streaming a few seconds before assuming anything.
		}

		UE_LOG(LogTemp, Warning,
			TEXT("[Spawn] No collision found under the drone after %.0fs. The Cesium tileset most "
			     "likely has 'Create Physics Meshes' disabled - ground traces cannot hit it. Placing "
			     "the drone %.0f m above the georeference origin instead, which is correct only if "
			     "the origin sits at ground level."),
			SpawnFallbackElapsed, SpawnHeightAboveGroundM);

		GroundZ = 0.0f;   // world origin
	}

	bSpawnHeightApplied = true;

	// Never in, against or facing into a building: raised until the pose is in open air.
	FVector Target(Current.X, Current.Y, GroundZ + SpawnHeightAboveGroundM * 100.0f);
	EPoseBlock Blocked = EPoseBlock::None;
	double RaisedM = 0.0;
	const bool bClear = RaiseUntilClear(Target, TargetThirdPersonCharacter->GetActorRotation(), Blocked, RaisedM);

	// Teleport rather than move: the character has a capsule and gravity, and a normal move from a
	// wrong height would either fall or refuse to penetrate the terrain it starts inside.
	TargetThirdPersonCharacter->SetActorLocation(Target, /*bSweep=*/false, nullptr,
		ETeleportType::TeleportPhysics);

	// The flight path is built around CharacterStartTransform, which is snapshotted at BeginPlay -
	// before this runs. Left alone, every waypoint keeps the drone's authored Z while the drone
	// itself sits at ground + SpawnHeightAboveGroundM, so the captures happen at a completely
	// different altitude from the one this function just chose. That is how a run ended up
	// photographing the city from 1.5 km up while the log reported a 50 m clearance.
	CharacterStartTransform.SetLocation(Target);

	LogSceneGeometry(TEXT("spawn"));

	UE_LOG(LogTemp, Log,
		TEXT("[Spawn] Drone placed %.0f m above the terrain (%.0f m asked%s): ground Z=%.0fcm, drone "
		     "Z=%.0fcm (was %.0fcm, a %.0f m correction). Flight path start moved with it."),
		(Target.Z - GroundZ) / 100.0, SpawnHeightAboveGroundM,
		RaisedM > 0.0 ? *FString::Printf(TEXT("; raised %.0f m, the asked height was %s"), RaisedM, PoseBlockText(Blocked)) : TEXT(""),
		GroundZ, Target.Z, Current.Z, (Target.Z - Current.Z) / 100.0f);
	if (!bClear)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Spawn] Still not in open air %.0f m up - check the start position."), MaxRaiseM);
	}
}


bool AMyActor_weather::ProbeGroundZ(double X, double Y, double NearZ, double& OutGroundZ) const
{
	OutGroundZ = 0.0;
	if (!GetWorld())
	{
		return false;
	}

	// From far above to far below, so the probe works whether the drone is above the terrain or
	// buried under it.
	const double TraceHalfSpan = 5.0e6;   // 50 km each way

	FCollisionQueryParams Params(SCENE_QUERY_STAT(WeatherPipelineGroundProbe), /*bTraceComplex=*/true);
	if (TargetThirdPersonCharacter)
	{
		Params.AddIgnoredActor(TargetThirdPersonCharacter);
	}

	// The point itself plus four around it. The photogrammetry mesh contains the buildings, so one
	// trace over a city lands on whatever roof is there; the lowest of five is the street unless every
	// sample happens to be a roof.
	const double R = FMath::Max(0.0f, GroundProbeRadiusM) * 100.0;
	TArray<FVector2D, TInlineAllocator<5>> Offsets;
	Offsets.Add(FVector2D(0.0, 0.0));
	if (R > 0.0)
	{
		Offsets.Add(FVector2D( R, 0.0));
		Offsets.Add(FVector2D(-R, 0.0));
		Offsets.Add(FVector2D(0.0,  R));
		Offsets.Add(FVector2D(0.0, -R));
	}

	bool bAnyHit = false;
	for (const FVector2D& Offset : Offsets)
	{
		const FVector Start(X + Offset.X, Y + Offset.Y, NearZ + TraceHalfSpan);
		const FVector End  (X + Offset.X, Y + Offset.Y, NearZ - TraceHalfSpan);

		FHitResult Hit;
		if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
		{
			OutGroundZ = bAnyHit ? FMath::Min(OutGroundZ, Hit.ImpactPoint.Z) : Hit.ImpactPoint.Z;
			bAnyHit = true;
		}
	}

	return bAnyHit;
}

bool AMyActor_weather::ResolveWaypointGround(FCaptureWaypoint& Waypoint)
{
	if (Waypoint.bGroundResolved)
	{
		return false;
	}
	Waypoint.bGroundResolved = true;

	double GroundZ = 0.0;
	if (!ProbeGroundZ(Waypoint.Location.X, Waypoint.Location.Y, Waypoint.Location.Z, GroundZ))
	{
		// Keep the flat-path altitude and say so. The metadata writes null for the ground fields of
		// this waypoint, so nothing downstream reads a clearance that was never measured.
		Waypoint.bGroundMeasured = false;
		UE_LOG(LogTemp, Warning,
			TEXT("[Terrain] No ground found under waypoint at (%.0f, %.0f); it keeps the flat-path "
			     "altitude and its ground fields are recorded as null."),
			Waypoint.Location.X, Waypoint.Location.Y);
		return false;
	}

	Waypoint.bGroundMeasured = true;
	Waypoint.GroundZ         = GroundZ;

	// Never in, against or facing into a building: raised until the capture pose is in open air.
	// Resolved once and reused, so every condition still shoots the identical pose.
	FVector Target(Waypoint.Location.X, Waypoint.Location.Y, GroundZ + Waypoint.HeightAboveGroundCm);
	EPoseBlock Blocked = EPoseBlock::None;
	double RaisedM = 0.0;
	const bool bClear = RaiseUntilClear(Target, Waypoint.Rotation, Blocked, RaisedM);
	const bool bMoved = FMath::Abs(Target.Z - Waypoint.Location.Z) > 1.0;   // > 1 cm
	Waypoint.Location.Z = Target.Z;

	UE_LOG(LogTemp, Log,
		TEXT("[Terrain] Waypoint at (%.0f, %.0f): ground Z=%.0fcm, flying %.1f m above it (%.1f m asked%s)."),
		Waypoint.Location.X, Waypoint.Location.Y, GroundZ, (Target.Z - GroundZ) / 100.0,
		Waypoint.HeightAboveGroundCm / 100.0f,
		RaisedM > 0.0 ? *FString::Printf(TEXT("; raised %.0f m, the asked height was %s"), RaisedM, PoseBlockText(Blocked)) : TEXT(""));
	if (!bClear)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Terrain] Waypoint at (%.0f, %.0f) still not in open air %.0f m up."),
			Waypoint.Location.X, Waypoint.Location.Y, MaxRaiseM);
	}

	return bMoved;
}

const TCHAR* AMyActor_weather::PoseBlockText(EPoseBlock Block)
{
	switch (Block)
	{
	case EPoseBlock::UnderSurface: return TEXT("inside or under a building");
	case EPoseBlock::Touching:     return TEXT("touching a building");
	case EPoseBlock::ViewBlocked:  return TEXT("facing a wall");
	default:                       return TEXT("clear");
	}
}

AMyActor_weather::EPoseBlock AMyActor_weather::CheckPose(const FVector& Location, const FRotator& Rotation) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return EPoseBlock::None;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WeatherPipelinePoseCheck), /*bTraceComplex=*/true);
	if (TargetThirdPersonCharacter)
	{
		Params.AddIgnoredActor(TargetThirdPersonCharacter);
	}

	constexpr double AboveEverythingCm = 5.0e6;   // 50 km: higher than any building or terrain
	constexpr float  ViewRayReach      = 1.0f / 3.0f;   // side rays a third of the way to the frame edge
	constexpr float  VerticalAspect    = 0.5f;          // the frame is about half as tall as wide
	constexpr int32  BlockedRays       = 3;             // of 5: a wall fills the frame

	// 1. Anything straight above the camera means it is inside a building or under a roof or bridge.
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Location + FVector(0.0, 0.0, AboveEverythingCm), Location,
		ECC_Visibility, Params))
	{
		return EPoseBlock::UnderSurface;
	}

	// 2. Geometry within the clearance radius: the camera is touching a wall.
	const float RadiusCm = CameraClearanceRadiusM * 100.0f;
	if (RadiusCm > 0.0f && World->SweepTestByChannel(Location, Location + FVector(0.0, 0.0, 1.0), FQuat::Identity,
		ECC_Visibility, FCollisionShape::MakeSphere(RadiusCm), Params))
	{
		return EPoseBlock::Touching;
	}

	// 3. The view: the centre ray and four around it, a third of the way to the frame edges. Three
	//    or more hitting within MinViewDistanceM means a wall fills the frame; one or two is a
	//    building beside the view, which is fine.
	const float Spread = CaptureFOV * ViewRayReach;
	const FRotator Offsets[] = { FRotator(0, 0, 0), FRotator(0, Spread, 0), FRotator(0, -Spread, 0),
	                             FRotator(Spread * VerticalAspect, 0, 0), FRotator(-Spread * VerticalAspect, 0, 0) };
	int32 Hits = 0;
	for (const FRotator& Offset : Offsets)
	{
		const FVector Direction = (Rotation.Quaternion() * Offset.Quaternion()).GetForwardVector();
		Hits += World->LineTraceTestByChannel(Location, Location + Direction * MinViewDistanceM * 100.0,
			ECC_Visibility, Params) ? 1 : 0;
	}
	return Hits >= BlockedRays ? EPoseBlock::ViewBlocked : EPoseBlock::None;
}

bool AMyActor_weather::RaiseUntilClear(FVector& Location, const FRotator& Rotation, EPoseBlock& OutFirst,
	double& OutRaisedM) const
{
	const double StartZ = Location.Z;
	const double StepCm = FMath::Max(1.0f, RaiseStepM) * 100.0;
	OutFirst = CheckPose(Location, Rotation);
	EPoseBlock Block = OutFirst;
	while (Block != EPoseBlock::None && Location.Z - StartZ + StepCm <= MaxRaiseM * 100.0)
	{
		Location.Z += StepCm;
		Block = CheckPose(Location, Rotation);
	}
	OutRaisedM = (Location.Z - StartZ) / 100.0;
	return Block == EPoseBlock::None;
}

bool AMyActor_weather::IsSegmentClear(const FVector& From, const FVector& To) const
{
	UWorld* World = GetWorld();
	if (!World || From.Equals(To, 1.0))
	{
		return true;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WeatherPipelineLegCheck), /*bTraceComplex=*/true);
	if (TargetThirdPersonCharacter)
	{
		Params.AddIgnoredActor(TargetThirdPersonCharacter);
	}
	return !World->SweepTestByChannel(From, To, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(FMath::Max(1.0f, CameraClearanceRadiusM * 100.0f)), Params);
}

TArray<FVector> AMyActor_weather::PlanLeg(const FVector& From, const FVector& To) const
{
	if (IsSegmentClear(From, To))
	{
		return {};
	}
	// Up above the start, across, down onto the target: both ends are in open air, so the vertical
	// parts are clear; only the crossing height has to be searched.
	const double StepCm = FMath::Max(1.0f, RaiseStepM) * 100.0;
	for (double Up = StepCm; Up <= MaxRaiseM * 100.0; Up += StepCm)
	{
		const double Z = FMath::Max(From.Z, To.Z) + Up;
		const FVector Above(From.X, From.Y, Z), Over(To.X, To.Y, Z);
		if (IsSegmentClear(From, Above) && IsSegmentClear(Above, Over) && IsSegmentClear(Over, To))
		{
			return { Above, Over };
		}
	}
	UE_LOG(LogTemp, Warning, TEXT("[Flight] No clear way found within %.0f m above the leg; flying it straight."), MaxRaiseM);
	return {};
}

FVector AMyActor_weather::PointAlongPath(const FVector& From, const TArray<FVector>& Points, double Alpha)
{
	double Total = 0.0;
	FVector Previous = From;
	for (const FVector& Point : Points)
	{
		Total += FVector::Dist(Previous, Point);
		Previous = Point;
	}
	double Remaining = FMath::Clamp(Alpha, 0.0, 1.0) * Total;
	Previous = From;
	for (const FVector& Point : Points)
	{
		const double Length = FVector::Dist(Previous, Point);
		if (Remaining <= Length && Length > 0.0)
		{
			return FMath::Lerp(Previous, Point, Remaining / Length);
		}
		Remaining -= Length;
		Previous = Point;
	}
	return Previous;
}

void AMyActor_weather::ResetTilesetOffsets()
{
	if (!bResetTilesetOffset)
	{
		return;
	}

	TArray<AActor*> Tilesets;
	GatherCesiumTilesets(Tilesets);

	for (AActor* Tileset : Tilesets)
	{
		USceneComponent* Root = Tileset ? Tileset->GetRootComponent() : nullptr;
		if (!Root)
		{
			continue;
		}

		const FVector Offset = Tileset->GetActorLocation();
		if (Offset.IsNearlyZero(1.0))
		{
			continue;
		}

		// The tileset root is Static, and a Static component refuses to move at runtime. Make it
		// movable for the move and put the mobility back, so nothing else about the actor changes.
		const EComponentMobility::Type Mobility = Root->Mobility;
		Root->SetMobility(EComponentMobility::Movable);
		Tileset->SetActorLocation(FVector::ZeroVector, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
		Root->SetMobility(Mobility);

		UE_LOG(LogTemp, Warning,
			TEXT("[Tileset] '%s' was offset to (%.0f, %.0f, %.0f) cm. Moved back to the origin for this "
			     "session: an offset tileset draws the terrain away from where the georeference puts it, "
			     "which makes every recorded altitude wrong by the offset. Set height with the "
			     "georeference's Origin Height instead, and save the tileset at 0,0,0."),
			*Tileset->GetName(), Offset.X, Offset.Y, Offset.Z);
	}
}

bool AMyActor_weather::GetGeoreferenceOrigin(double& OutLatitude, double& OutLongitude, double& OutHeight) const
{
	UClass* GeoClass = LoadObject<UClass>(nullptr, TEXT("/Script/CesiumRuntime.CesiumGeoreference"));
	AActor* Georeference = GeoClass ? UGameplayStatics::GetActorOfClass(GetWorld(), GeoClass) : nullptr;
	if (!Georeference)
	{
		return false;
	}
	auto ReadDouble = [Georeference](const TCHAR* Name, double& Out) -> bool
	{
		const FDoubleProperty* Prop = CastField<FDoubleProperty>(Georeference->GetClass()->FindPropertyByName(Name));
		if (Prop)
		{
			Out = Prop->GetPropertyValue_InContainer(Georeference);
		}
		return Prop != nullptr;
	};
	return ReadDouble(TEXT("OriginLatitude"), OutLatitude) && ReadDouble(TEXT("OriginLongitude"), OutLongitude) &&
		ReadDouble(TEXT("OriginHeight"), OutHeight);
}

bool AMyActor_weather::SetGeoreferenceOriginHeight(double HeightM)
{
	double Latitude = 0.0, Longitude = 0.0, Height = 0.0;
	UClass* GeoClass = LoadObject<UClass>(nullptr, TEXT("/Script/CesiumRuntime.CesiumGeoreference"));
	AActor* Georeference = GeoClass ? UGameplayStatics::GetActorOfClass(GetWorld(), GeoClass) : nullptr;
	UFunction* SetOrigin = Georeference ? Georeference->FindFunction(TEXT("SetOriginLongitudeLatitudeHeight")) : nullptr;
	if (!SetOrigin || !GetGeoreferenceOrigin(Latitude, Longitude, Height))
	{
		return false;
	}
	// Longitude first: SetOriginLongitudeLatitudeHeight takes Unreal's X/Y/Z order.
	struct FSetOriginParams { FVector TargetLongitudeLatitudeHeight; };
	FSetOriginParams Params;
	Params.TargetLongitudeLatitudeHeight = FVector(Longitude, Latitude, HeightM);
	Georeference->ProcessEvent(SetOrigin, &Params);
	return true;
}

bool AMyActor_weather::ShiftGeoreferenceOriginHeight(double DeltaM)
{
	double Latitude = 0.0, Longitude = 0.0, Height = 0.0;
	if (!GetGeoreferenceOrigin(Latitude, Longitude, Height) || !SetGeoreferenceOriginHeight(Height + DeltaM))
	{
		return false;
	}
	UE_LOG(LogTemp, Log,
		TEXT("[Site] Ground measured %.1f m from Z = 0; georeference Origin Height %.1f -> %.1f m "
		     "(ellipsoid), so Z = 0 is the ground here."), DeltaM, Height, Height + DeltaM);
	return true;
}

void AMyActor_weather::StartGroundHeightCalculation()
{
#if WITH_CESIUM_RUNTIME
	double Latitude = 0.0, Longitude = 0.0, Height = 0.0;
	ACesium3DTileset* Tileset = nullptr;
	for (TActorIterator<ACesium3DTileset> It(GetWorld()); It && !Tileset; ++It)
	{
		Tileset = *It;
	}
	if (!bCalculateOriginHeight || !Tileset || !GetGeoreferenceOrigin(Latitude, Longitude, Height))
	{
		return;   // the collision-probe calibration in ApplySpawnHeight covers this case
	}

	// The site and four points GroundProbeRadiusM away, converted from metres to degrees.
	const double RadiusM  = FMath::Max(0.0f, GroundProbeRadiusM);
	const double DegLat   = RadiusM / 111320.0;
	const double DegLon   = RadiusM / (111320.0 * FMath::Max(0.01, FMath::Cos(FMath::DegreesToRadians(Latitude))));
	const TArray<FVector> Positions = {
		FVector(Longitude, Latitude, 0.0),
		FVector(Longitude + DegLon, Latitude, 0.0), FVector(Longitude - DegLon, Latitude, 0.0),
		FVector(Longitude, Latitude + DegLat, 0.0), FVector(Longitude, Latitude - DegLat, 0.0) };

	bGroundCalculationPending = true;
	TWeakObjectPtr<AMyActor_weather> WeakThis(this);
	Tileset->SampleHeightMostDetailed(Positions, FCesiumSampleHeightMostDetailedCallback::CreateLambda(
		[WeakThis](ACesium3DTileset*, const TArray<FCesiumSampleHeightResult>& Results, const TArray<FString>& Warnings)
		{
			if (!WeakThis.IsValid())
			{
				return;
			}
			TArray<double> Heights;
			for (const FCesiumSampleHeightResult& Result : Results)
			{
				if (Result.SampleSuccess)
				{
					Heights.Add(Result.LongitudeLatitudeHeight.Z);
				}
			}
			for (const FString& Warning : Warnings)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Site] Height sampling: %s"), *Warning);
			}
			WeakThis->FinishGroundHeightCalculation(Heights);
		}));
	UE_LOG(LogTemp, Log, TEXT("[Site] Calculating the ground height at %.6f, %.6f from the tileset..."),
		Latitude, Longitude);
#endif
}

void AMyActor_weather::FinishGroundHeightCalculation(const TArray<double>& EllipsoidHeights)
{
	bGroundCalculationPending = false;
	double Latitude = 0.0, Longitude = 0.0, Previous = 0.0;
	if (EllipsoidHeights.Num() == 0 || !GetGeoreferenceOrigin(Latitude, Longitude, Previous))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Site] The tileset returned no ground height; measuring it with the collision probe instead."));
		return;
	}

	const double Ground = FMath::Min(EllipsoidHeights);
	if (!SetGeoreferenceOriginHeight(Ground))
	{
		return;
	}
	bOriginFromCalculation    = true;
	OriginCalibratedAtSeconds = GetWorld()->GetTimeSeconds();   // spawn waits for the tiles to move

	UE_LOG(LogTemp, Log,
		TEXT("[Site] Ground height calculated from the tileset: %.2f m above the ellipsoid (%.2f m above sea "
		     "level, lowest of %d samples); georeference Origin Height %.2f -> %.2f m, so Z = 0 is the ground."),
		Ground, Ground - GetGeoidSeparationAt(Latitude, Longitude), EllipsoidHeights.Num(), Previous, Ground);
}

void AMyActor_weather::LogSceneGeometry(const TCHAR* When) const
{
	if (!TargetThirdPersonCharacter || !GetWorld())
	{
		return;
	}

	const FVector Drone = TargetThirdPersonCharacter->GetActorLocation();
	double LocalGroundZ = 0.0;
	if (!ProbeGroundZ(Drone.X, Drone.Y, Drone.Z, LocalGroundZ))
	{
		UE_LOG(LogTemp, Log, TEXT("[Scene] %s: no ground under the drone - geometry not logged."), When);
		return;
	}

	// Layers are placed against the spawn ground, so they are reported against it too.
	const double GroundZ = bHaveSpawnGround ? SpawnGroundZ : LocalGroundZ;
	auto AboveGround = [GroundZ](double Z) { return (Z - GroundZ) / 100.0; };   // cm -> m

	FString Fog = TEXT("none");
	if (const UExponentialHeightFogComponent* FogComp = GetSkyHeightFogComponent())
	{
		Fog = FString::Printf(TEXT("plane %+.0f m, density %.4f, falloff %.3f, start %.0f m, visible %s"),
			AboveGround(FogComp->GetComponentLocation().Z), FogComp->FogDensity, FogComp->FogHeightFalloff,
			FogComp->StartDistance / 100.0f, FogComp->IsVisible() ? TEXT("yes") : TEXT("no"));
	}

	FString Atmosphere = TEXT("none");
	if (AActor* UDS = UDSActor.Get())
	{
		if (const USkyAtmosphereComponent* Atmo = UDS->FindComponentByClass<USkyAtmosphereComponent>())
		{
			const double AtmoZ = Atmo->GetComponentLocation().Z;
			const double Surface =
				Atmo->TransformMode == ESkyAtmosphereTransformMode::PlanetTopAtAbsoluteWorldOrigin ? 0.0 :
				Atmo->TransformMode == ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform   ? AtmoZ :
				AtmoZ + static_cast<double>(Atmo->BottomRadius) * 100000.0;
			Atmosphere = FString::Printf(TEXT("surface %+.0f m (mode %d, radius %.0f km, aerial perspective x%.2f)"),
				AboveGround(Surface), static_cast<int32>(Atmo->TransformMode), Atmo->BottomRadius,
				Atmo->AerialPespectiveViewDistanceScale);
		}

		// The renderer's own cloud layer, which is what decides where the clouds are drawn.
		if (const UVolumetricCloudComponent* Cloud = UDS->FindComponentByClass<UVolumetricCloudComponent>())
		{
			Atmosphere += FString::Printf(TEXT(", cloud layer bottom %.3f km, height %.3f km above that surface"),
				Cloud->LayerBottomAltitude, Cloud->LayerHeight);
		}
	}

	// How high the sun stands and where from: what the day-night cycle and the spot's time really did.
	FString Sun;
	float SunElevationDeg = 0.0f, SunFromCompassDeg = 0.0f;
	if (GetSunDirection(SunElevationDeg, SunFromCompassDeg))
	{
		Sun = FString::Printf(TEXT(" | sun %+.1f deg, from compass %.0f deg at time %.0f"),
			SunElevationDeg, SunFromCompassDeg, ActiveCaptureTimeOfDay);
	}

	UE_LOG(LogTemp, Log,
		TEXT("[Scene] %s, heights above the ground: drone %+.0f m, clearance %+.0f m, world Z=0 %+.0f m | height fog %s | atmosphere %s%s"),
		When, AboveGround(Drone.Z), (Drone.Z - LocalGroundZ) / 100.0, AboveGround(0.0), *Fog, *Atmosphere, *Sun);
}

bool AMyActor_weather::GetSunDirection(float& OutElevationDeg, float& OutFromCompassDeg) const
{
	AActor* UDS = UDSActor.Get();
	if (!UDS)
	{
		return false;
	}
	TArray<UDirectionalLightComponent*> Lights;
	UDS->GetComponents(Lights);
	for (const UDirectionalLightComponent* Light : Lights)
	{
		if (Light->GetName().Contains(TEXT("Sun")))
		{
			// A light shines along its forward vector, so the sun is the opposite way: its elevation is
			// minus the light's pitch; +X is east, so the compass bearing is Unreal yaw + 90.
			const FRotator Shine = Light->GetComponentRotation();
			OutElevationDeg   = -Shine.Pitch;
			OutFromCompassDeg = FMath::Fmod(Shine.Yaw + 270.0 + 720.0, 360.0);
			return true;
		}
	}
	return false;
}
